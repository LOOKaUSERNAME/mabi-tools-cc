#include "service_flow.hpp"
#include <QJsonArray>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cc::service_flow {
namespace {
constexpr qint64 hour = 3'600'000;

bool is_notice(source feed) {
    return feed == source::maintenance;
}

QDateTime completed_at(const QJsonObject& entry) {
    return QDateTime::fromString(entry.value("completedUtc").toString(), Qt::ISODate);
}
}

void coordinator::advance(qint64 elapsed_ms, const QDateTime& utc) {
    if (elapsed_ms < time_ || elapsed_ms > std::numeric_limits<qint64>::max() - hour ||
        !utc.isValid())
        throw std::invalid_argument("Expected a monotonic elapsed clock and valid wall clock.");
    time_ = elapsed_ms;
    now_ = utc.toUTC();
    if (update_ && time_ >= update_->deadline) {
        const auto feed = update_->feed;
        abort(update_);
        failed(feed, true);
    }
    if (pending_update_ && update_end_ && time_ >= *update_end_) {
        abort(update_);
        failed(source::update_primary, false);
    }
    if (notice_ && time_ >= notice_->deadline) {
        abort(notice_);
        failed(source::maintenance, false);
    }
    if ((maintenance_ == notice_status::available || maintenance_ == notice_status::uncertain) &&
        last_notice_success_ && time_ - *last_notice_success_ >= hour)
        update_notice_status();
    pump();
}

void coordinator::restore_confirmed(const QJsonObject& event, const QDateTime& fetched,
                                    bool uncertain) {
    if (!now_.isValid() || !fetched.isValid() || fetched > now_ || last_notice_success_ ||
        enabled_ || relevant_ || notice_ || closing_)
        throw std::logic_error("Restore the validated cache before enabling maintenance.");
    confirmed_ = event;
    notice_uncertain_ = uncertain;
    last_notice_wall_ = fetched;
    last_notice_attempt_ = last_notice_success_ = time_ - std::min(hour, fetched.msecsTo(now_));
    update_notice_status();
}

void coordinator::reset_update() {
    update_attempt_ = 0;
    update_due_.reset();
    update_end_.reset();
}

void coordinator::request_update() {
    if (!updates_allowed_ || closing_ || pending_update_ || pending_download_ || download_)
        return;
    pending_update_ = true;
    metadata_ = {};
    ready_file_.clear();
    update_error_.clear();
    reset_update();
    updates_ = scan_ == scan_status::idle ? update_status::waiting : update_status::paused;
    pump();
}

void coordinator::maintenance_context(bool enabled, bool history_relevant) {
    if (closing_)
        return;
    enabled = enabled && maintenance_allowed_;
    if (enabled_ == enabled && relevant_ == history_relevant)
        return;
    enabled_ = enabled;
    relevant_ = history_relevant;
    if (!enabled_ || !relevant_)
        abort(notice_);
    update_notice_status();
    pump();
}

// Retire active network requests but retain pending intent. Readiness is granted
// by pump only after transport drain acknowledgements, never by abort returning.
void coordinator::request_scan() {
    if (closing_ || scan_ != scan_status::idle)
        return;
    scan_ = scan_status::draining;
    abort(update_);
    abort(notice_);
    abort_download();
    if (pending_update_) {
        reset_update();
        updates_ = update_status::paused;
    }
    if (pending_download_)
        updates_ = update_status::paused;
    update_notice_status();
    pump();
}

void coordinator::scan_started() {
    if (scan_ != scan_status::ready)
        throw std::logic_error("Wait for feed requests to drain before starting probes.");
    scan_ = scan_status::running;
}

void coordinator::scan_finished() {
    if (closing_ || scan_ == scan_status::idle)
        return;
    scan_ = scan_status::idle;
    update_notice_status();
    pump();
}

// Close is permanent: clear pending intent before aborting, so late callbacks or
// scan_finished cannot restart services. The owner keeps the event loop running
// until closed() confirms every retired request has released its resources.
void coordinator::close() {
    if (closing_)
        return;
    closing_ = true;
    pending_update_ = false;
    pending_download_ = false;
    reset_update();
    abort(update_);
    abort(notice_);
    abort_download();
}

// Central admission gate for both resumed work and new requests. A scan becomes
// ready only when drains are empty, and network work starts only while idle.
void coordinator::pump() {
    if (closing_ || !now_.isValid() || !draining_.empty())
        return;
    if (scan_ == scan_status::draining)
        scan_ = scan_status::ready;
    if (scan_ != scan_status::idle)
        return;
    if (pending_download_ && !download_) {
        download_ = ++next_id_;
        updates_ = update_status::downloading;
        adapter_.start_download(*download_, metadata_.value("installer").toObject());
    }
    if (pending_update_ && !update_ && (!update_due_ || time_ >= *update_due_)) {
        if (!update_end_)
            update_end_ = time_ + 20'000;
        const auto feed = update_attempt_ == 1 ? source::update_fallback : source::update_primary;
        updates_ = update_status::fetching;
        start(update_, feed, std::min<qint64>(5'000, *update_end_ - time_), 64 * 1024);
    }
    if (enabled_ && relevant_ && !notice_ &&
        (!last_notice_attempt_ || time_ - *last_notice_attempt_ >= hour)) {
        maintenance_ = notice_status::fetching;
        notice_started_ = time_;
        start(notice_, source::maintenance, 12'000, 1024 * 1024);
    }
}

void coordinator::start(std::optional<reply>& slot, source feed, qint64 duration,
                        qsizetype maximum) {
    slot = reply{++next_id_, feed, time_ + duration, maximum, false, {}};
    adapter_.start(slot->id, feed);
}

void coordinator::abort(std::optional<reply>& slot) {
    if (!slot)
        return;
    const auto id = slot->id;
    // Retire before asking the adapter to abort: late data must not find an
    // active slot, and even an immediate acknowledgement must see this drain.
    slot.reset();
    draining_.insert(id);
    adapter_.abort(id);
}

void coordinator::abort_download() {
    if (!download_)
        return;
    const auto id = *download_;
    download_.reset();
    draining_.insert(id);
    adapter_.abort(id);
}

void coordinator::download_complete(request_id id, bool verified, const QString& path,
                                    const QString& error) {
    if (closing_ || !download_ || *download_ != id)
        return;
    download_.reset();
    pending_download_ = false;
    ready_file_ = verified ? path : QString{};
    update_error_ = error;
    updates_ = verified && !path.isEmpty() ? update_status::ready : update_status::failed;
    pump();
}

std::optional<coordinator::reply>* coordinator::find(request_id id) {
    if (closing_)
        return nullptr;
    if (update_ && update_->id == id)
        return &update_;
    if (notice_ && notice_->id == id)
        return &notice_;
    return nullptr;
}

void coordinator::drained(request_id id) {
    if (draining_.erase(id))
        pump();
}

void coordinator::headers(request_id id, int status, std::optional<qint64> length) {
    auto* slot = find(id);
    if (!slot)
        return;
    const auto feed = (*slot)->feed;
    const bool ok = !(*slot)->header && status >= 200 && status < 300 &&
                    (!length || (*length >= 0 && *length <= (*slot)->maximum));
    if (!ok) {
        abort(*slot);
        // Redirects and non-success statuses never deliver content. The update
        // adapter may try its fixed alternate source, not the redirect target.
        failed(feed, status >= 300 && status <= 599);
        return;
    }
    (*slot)->header = true;
}

void coordinator::body(request_id id, const QByteArray& bytes) {
    auto* slot = find(id);
    if (!slot)
        return;
    if (!(*slot)->header || bytes.size() > (*slot)->maximum - (*slot)->bytes.size()) {
        const auto feed = (*slot)->feed;
        abort(*slot);
        failed(feed, false);
        return;
    }
    (*slot)->bytes.append(bytes);
}

void coordinator::complete(request_id id, bool transport_ok) {
    if (draining_.contains(id))
        return; // Only the adapter's explicit drain acknowledgement grants admission.
    auto* slot = find(id);
    if (!slot)
        return;
    const auto result = std::move(**slot);
    slot->reset();
    if (!transport_ok || !result.header) {
        failed(result.feed, true);
        pump();
        return;
    }
    try {
        if (is_notice(result.feed)) {
            publish_notice(service_policy::maintenance(result.bytes, now_));
        } else {
            metadata_ = mode_ == update_mode::metadata_only
                            ? service_policy::update(result.bytes, installed_, commit_)
                            : service_policy::native_update(result.bytes, installed_);
            updates_ = metadata_.value("available").toBool() ? update_status::available
                                                             : update_status::current;
            pending_update_ = false;
            pending_download_ =
                mode_ == update_mode::installed && metadata_.value("available").toBool();
            reset_update();
        }
    } catch (const std::invalid_argument&) {
        failed(result.feed, false);
    }
    pump();
}

void coordinator::failed(source feed, bool retryable) {
    if (is_notice(feed)) {
        last_notice_attempt_ = notice_started_;
        maintenance_ = notice_status::unavailable;
        return; // Retain the last confirmed completion, even with a bad response.
    }
    if (retryable && update_attempt_ < 2 && update_end_ && time_ < *update_end_) {
        ++update_attempt_;
        update_due_ = time_ + (update_attempt_ == 1 ? 1'000 : 3'000);
        updates_ = update_status::waiting;
    } else {
        pending_update_ = false;
        reset_update();
        updates_ = update_status::failed;
    }
}

void coordinator::update_notice_status() {
    if (!enabled_)
        maintenance_ = notice_status::disabled;
    else if (!relevant_)
        maintenance_ = notice_status::paused;
    else if (!last_notice_success_)
        maintenance_ = notice_status::unknown;
    else if (time_ - *last_notice_success_ >= hour)
        maintenance_ = notice_status::stale;
    else
        maintenance_ = notice_uncertain_ ? notice_status::uncertain : notice_status::available;
}

void coordinator::publish_notice(const QJsonObject& parsed) {
    const auto events = parsed.value("events").toArray();
    // A correction to the last confirmed notice may move its date backward.
    // Then select the newest other confirmed event, if one exists.
    auto next = confirmed_;
    if (!next.isEmpty()) {
        for (const auto& event : events) {
            if (event.toObject().value("sourceId") == next.value("sourceId")) {
                next = event.toObject();
                break;
            }
        }
    }
    for (const auto& event : events) {
        const auto candidate = event.toObject();
        if (next.isEmpty() || completed_at(candidate) > completed_at(next))
            next = candidate;
    }
    confirmed_ = std::move(next);
    last_notice_attempt_ = notice_started_;
    last_notice_success_ = time_;
    last_notice_wall_ = now_;
    notice_uncertain_ = confirmed_.isEmpty() || parsed.value("uncertain").toBool();
    update_notice_status();
}
}
