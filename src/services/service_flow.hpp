#pragma once
#include "service_policy.hpp"
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <stdexcept>

namespace cc::service_flow {
using request_id = std::uint64_t;
enum class source { update_primary, update_fallback, maintenance };
enum class update_status {
    idle,
    fetching,
    waiting,
    paused,
    current,
    available,
    downloading,
    ready,
    failed
};
enum class update_mode { metadata_only, portable, installed };
enum class notice_status {
    unknown,
    fetching,
    available,
    uncertain,
    unavailable,
    disabled,
    paused,
    stale
};
enum class scan_status { idle, draining, ready, running };

struct transport {
    virtual ~transport() = default;
    // Callbacks must be queued onto the coordinator's thread, never synchronous.
    // A drain signal must mean the complete request/body has been released.
    virtual void start(request_id id, source feed) = 0;
    virtual void abort(request_id id) = 0;

    virtual void start_download(request_id, const QJsonObject&) {
        throw std::logic_error("Downloads unavailable in this transport");
    }
};

// Single-threaded service state machine; call it on the transport's event-loop
// thread. The borrowed adapter must outlive it. A scan request retires metadata
// and downloads, then waits for every drain acknowledgement before becoming
// ready. scan_finished resumes pending work once; close permanently suppresses
// restarts. Keep the event loop alive until closed() before destroying either.
class coordinator {
public:
    explicit coordinator(transport& adapter, QString installed, QString commit = {},
                         bool updates_allowed = true, update_mode mode = update_mode::metadata_only,
                         bool maintenance_allowed = true)
        : adapter_(adapter), installed_(std::move(installed)), commit_(std::move(commit)),
          updates_allowed_(updates_allowed), mode_(mode),
          maintenance_allowed_(maintenance_allowed) {}

    coordinator(const coordinator&) = delete;
    coordinator& operator=(const coordinator&) = delete;

    // Update the monotonic elapsed clock before dispatching any received frame.
    // An observed frame at its deadline loses to timeout.
    void advance(qint64 elapsed_ms, const QDateTime& utc);
    void request_update();
    void maintenance_context(bool enabled, bool history_relevant);
    // request_scan is asynchronous: wait for scan()==ready, then call
    // scan_started before probing. scan_finished also cancels a pending scan.
    void request_scan();
    void scan_started();
    void scan_finished();
    void close();
    void headers(request_id, int status, std::optional<qint64> length = {});
    void body(request_id, const QByteArray& bytes);
    void complete(request_id, bool transport_ok);
    void drained(request_id);
    void download_complete(request_id, bool verified, const QString& path, const QString& error);

    update_mode mode() const { return mode_; }

    const QString& ready_file() const { return ready_file_; }

    const QString& update_error() const { return update_error_; }

    bool updates_allowed() const { return updates_allowed_; }

    update_status updates() const { return updates_; }

    notice_status maintenance() const { return maintenance_; }

    scan_status scan() const { return scan_; }

    // Restore persisted maintenance before enabling its context. Failed fetches
    // do not erase this event: history comparisons still need the last boundary.
    void restore_confirmed(const QJsonObject&, const QDateTime& fetched, bool uncertain);

    QDateTime notice_retrieved() const { return last_notice_wall_; }

    bool notice_uncertain() const { return notice_uncertain_; }

    const QJsonObject& confirmed() const { return confirmed_; }

    const QJsonObject& update_metadata() const { return metadata_; }

    bool closed() const { return closing_ && draining_.empty(); }

private:
    struct reply {
        request_id id;
        source feed;
        qint64 deadline;
        qsizetype maximum;
        bool header{};
        QByteArray bytes;
    };

    transport& adapter_;
    QString installed_, commit_;
    const bool updates_allowed_;
    const update_mode mode_;
    const bool maintenance_allowed_;
    std::optional<request_id> download_;
    bool pending_download_{};
    QString ready_file_, update_error_;
    qint64 time_{}, notice_started_{};
    QDateTime now_, last_notice_wall_;
    request_id next_id_{};
    std::optional<reply> update_, notice_;
    std::set<request_id> draining_;
    std::optional<qint64> update_due_, update_end_, last_notice_attempt_, last_notice_success_;
    update_status updates_{update_status::idle};
    notice_status maintenance_{notice_status::unknown};
    scan_status scan_{scan_status::idle};
    bool enabled_{}, relevant_{}, closing_{}, pending_update_{}, notice_uncertain_{};
    int update_attempt_{};
    QJsonObject confirmed_, metadata_;

    void pump();
    void start(std::optional<reply>&, source, qint64 duration, qsizetype maximum);
    void abort(std::optional<reply>&);
    void abort_download();
    std::optional<reply>* find(request_id);
    void failed(source feed, bool retryable);
    void reset_update();
    void update_notice_status();
    void publish_notice(const QJsonObject&);
};
}
