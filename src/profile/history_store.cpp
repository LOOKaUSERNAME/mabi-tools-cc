// Native profile storage: bounded report decoding, durable save admission and
// one serialised disk queue. The profile lock coordinates multiple processes;
// epoch/generation checks prevent a completed scan from undoing Clear or Off.
// Retention consent is revalidated against actual files before removal.

#include "history_store.hpp"
#include "profile_paths.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QStringList>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <ranges>

namespace cc::ui {
namespace {
using namespace std::chrono_literals;
constexpr qint64 record_limit = 4 * 1024 * 1024;
#ifndef CC_PROFILE_HISTORY
constexpr int record_count_limit = 24;
#endif
#ifdef CC_PROFILE_HISTORY
// Persisted format identifier: changing it would make existing profiles unreadable.
constexpr auto namespace_tag = "MabiTools.Qt.CandidateHistory.1";
#else
constexpr auto namespace_tag = "MabiTools.Qt.LoopbackHistoryPreview.1";
#endif
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QString uuid() {
    return QUuid::createUuid().toString(QUuid::Id128);
}

bool valid_id(const QString& text) {
    static const QRegularExpression id(QStringLiteral("\\A[0-9a-f]{32}\\z"));
    return id.match(text).hasMatch();
}

const wchar_t* wide(const QString& path) {
    return reinterpret_cast<const wchar_t*>(path.utf16());
}

QString checked_root(const QString& root) {
    const QFileInfo info(root);
    require(
        info.isDir() && !info.isSymLink() && !info.isJunction() &&
            info.fileName().startsWith("mabitools-cc-history-fixture-") &&
            QDir(info.absolutePath())
                    .canonicalPath()
                    .compare(QDir(QDir::tempPath()).canonicalPath(), Qt::CaseInsensitive) == 0 &&
            info.canonicalFilePath().compare(QDir(QDir::tempPath()).canonicalPath() + '/' +
                                                 info.fileName(),
                                             Qt::CaseInsensitive) == 0,
        "Native history requires a disposable history fixture folder directly under system temp.");
    return info.canonicalFilePath();
}

void check_path(const QString& path, bool directory, bool missing_ok = false) {
    const auto attributes = GetFileAttributesW(wide(path));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        require(missing_ok && GetLastError() == ERROR_FILE_NOT_FOUND,
                "Cannot access the history path.");
        return;
    }
    require(!(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
                ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory,
            "Unexpected history path type or reparse point.");
}

class scoped_handle {
public:
    explicit scoped_handle(HANDLE handle) : handle_(handle) {}

    ~scoped_handle() {
        if (valid())
            CloseHandle(handle_);
    }

    scoped_handle(const scoped_handle&) = delete;

    scoped_handle(scoped_handle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

// Acquire an exclusive OS handle across processes, independently of queue_gate_.
// Retry only lock contention, with a bounded wait and cancellation between tries.
// Validate the opened handle too: the pathname check alone cannot establish that
// the lock is an ordinary, singly linked file. Closing the handle releases it.
scoped_handle open_history_lock(const QString& root, bool persistent_profile,
                                std::stop_token stop = {}) {
    const auto path = QDir(persistent_profile ? checked_profile_root(root) : checked_root(root))
                          .filePath("history.lock");
    check_path(path, false, true);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!stop.stop_requested()) {
        const auto opened = CreateFileW(wide(path), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (opened != INVALID_HANDLE_VALUE) {
            scoped_handle result(opened);
            BY_HANDLE_FILE_INFORMATION info{};
            require(GetFileInformationByHandle(opened, &info) &&
                        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                        info.nNumberOfLinks == 1,
                    "Invalid native history lock.");
            return result;
        }
        const auto error = GetLastError();
        require(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION,
                "Cannot open scan history lock.");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Timed out waiting for scan history lock.");
        std::this_thread::sleep_for(20ms);
    }
    return scoped_handle(INVALID_HANDLE_VALUE);
}

QByteArray read_bounded(const QString& path, qint64 limit) {
    check_path(path, false);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot open scan history file.");
    const auto bytes = file.read(limit + 1);
    require(file.error() == QFileDevice::NoError && bytes.size() <= limit,
            "Cannot read the history file within its size limit.");
    return bytes;
}

void replace_preferences(const QString& path, const QByteArray& bytes) {
    check_path(path, false, true);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
            "Cannot replace scan history preferences.");
}

// Stage and flush a bounded report, then publish by an atomic no-replace rename.
// The caller holds the profile lock; still_admitted catches local invalidation
// during I/O. The scope guard removes an unpublished temporary on every exit.
void publish_record(const QString& folder, const QString& destination, const QByteArray& bytes,
                    const std::function<bool()>& still_admitted) {
    check_path(folder, true);
    require(bytes.size() > 0 && bytes.size() <= record_limit,
            "Scan history record exceeded the size limit.");
    const auto stage = QDir(folder).filePath("pending-" + uuid() + ".tmp");

    struct cleanup {
        QString path;
        bool own{};

        ~cleanup() {
            if (own)
                DeleteFileW(wide(path));
        }
    } staged{stage};

    {
        scoped_handle temporary(CreateFileW(wide(stage), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr));
        require(temporary.valid(), "Cannot create history stage.");
        staged.own = true;
        const auto size = static_cast<DWORD>(bytes.size());
        DWORD total{};
        while (total < size) {
            DWORD written{};
            require(WriteFile(temporary.get(), bytes.constData() + total, size - total, &written,
                              nullptr) != 0 &&
                        written != 0,
                    "Cannot write history stage.");
            total += written;
        }
        require(FlushFileBuffers(temporary.get()) != 0, "Cannot flush history stage.");
    }
    // WPF-style last generation check before atomic no-replace rename. Work
    // already inside the single OS rename can finish before a new Off/Clear;
    // Clear then removes owned records after it acquires the same lock.
    if (!still_admitted())
        return;
    require(MoveFileExW(wide(stage), wide(destination), MOVEFILE_WRITE_THROUGH) != 0,
            "Cannot publish an immutable history record.");
    staged.own = false;
}

QStringList owned(const QString& folder) {
    check_path(folder, true);
    static const QRegularExpression name(QStringLiteral("\\Anative-scan-([0-9a-f]{32})\\.json\\z"));
    QStringList records;
    for (const auto& file :
         QDir(folder).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks)) {
        const auto match = name.match(file.fileName());
        if (!match.hasMatch())
            continue;
        const auto path = file.absoluteFilePath();
        try {
            const auto obj = QJsonDocument::fromJson(read_bounded(path, record_limit)).object();
            if (obj["Format"] == namespace_tag && obj["Id"].toString() == match.captured(1) &&
                obj["Report"].isString()) {
                records << path;
#ifndef CC_PROFILE_HISTORY
                require(records.size() <= record_count_limit,
                        "Too many native history fixture files.");
#endif
            }
        } catch (const std::runtime_error&) {
            // An unreadable/unknown record is not owned by this store.
#ifndef CC_PROFILE_HISTORY
            if (records.size() > record_count_limit)
                throw;
#endif
        }
    }
    return records;
}
}

history_store::history_store(const QString& root, bool persistent_profile)
    : root_(persistent_profile ? checked_profile_root(root) : checked_root(root)),
      persistent_profile_(persistent_profile) {
    enqueue([this] {
        const auto guard = open_history_lock(root_, persistent_profile_);
        const auto path = folder();
        if (!QDir(path).exists())
            require(QDir(root_).mkdir("native-history"), "Cannot create scan history.");
        check_path(path, true);
        auto prefs = load_preferences();
        if (!QFile::exists(QDir(root_).filePath("native-history-preferences.json")))
            store_preferences(prefs);
        return inspect(prefs);
    });
    worker_ = std::jthread([this] { run(); });
}

history_store::~history_store() {
    drain();
}

QString history_store::folder() const {
    return QDir(root_).filePath("native-history");
}

history_store::admission history_store::capture() const {
    return {generation_.load(), QDateTime::currentMSecsSinceEpoch()};
}

history_store::preferences history_store::load_preferences() const {
    const auto path = QDir(root_).filePath("native-history-preferences.json");
    check_path(path, false, true);
    if (!QFile::exists(path))
        return {true, -1, uuid()};
    const auto obj = QJsonDocument::fromJson(read_bounded(path, 16 * 1024)).object();
    require(obj["Format"] == namespace_tag && obj["Saving"].isBool() &&
                obj["InvalidatedUtcMs"].isDouble() && obj["InvalidatedUtcMs"].toInteger(-2) >= -1 &&
                valid_id(obj["Epoch"].toString()),
            "Unsupported scan history preferences.");
    limits value;
#ifdef CC_PROFILE_HISTORY
    const auto days = obj.value("KeepDays"), mib = obj.value("StoreMiB");
    if (!days.isUndefined() || !mib.isUndefined()) {
        value = {days.toInt(-1), mib.toInt(-1)};
        require((value.days == 0 || value.days == 7 || value.days == 30 || value.days == 90) &&
                    (value.mib == 100 || value.mib == 250 || value.mib == 500),
                "Unsupported native history limits.");
    }
#endif
    return {obj["Saving"].toBool(), obj["InvalidatedUtcMs"].toInteger(), obj["Epoch"].toString(),
            value};
}

void history_store::store_preferences(const preferences& prefs) const {
    const auto path = QDir(root_).filePath("native-history-preferences.json");
    QJsonObject values{{"Format", namespace_tag},
                       {"Saving", prefs.saving},
                       {"InvalidatedUtcMs", prefs.invalidated_ms},
                       {"Epoch", prefs.epoch}};
#ifdef CC_PROFILE_HISTORY
    values["KeepDays"] = prefs.retention.days;
    values["StoreMiB"] = prefs.retention.mib;
#endif
    const auto data = QJsonDocument(values).toJson(QJsonDocument::Compact);
    replace_preferences(path, data);
}
#ifdef CC_PROFILE_HISTORY
namespace {
std::int64_t utc_ticks(const QDateTime& value) {
    return 621355968000000000LL + value.toMSecsSinceEpoch() * 10'000LL;
}

std::int64_t report_ticks(const QJsonValue& value) {
    const auto raw = value.toString();
    const auto date = QDateTime::fromString(raw, Qt::ISODateWithMs);
    static const QRegularExpression form(R"((?:\.(\d{1,7}))?(?:Z|[+-]\d\d:\d\d)$)");
    const auto match = form.match(raw);
    require(date.isValid() && date.date().year() >= 2004 && match.hasMatch(),
            "Invalid saved scan timestamp.");
    const auto fraction = match.captured(1).leftJustified(7, '0').toInt();
    return utc_ticks(date) + fraction % 10'000;
}

std::optional<double> report_timing(const QJsonValue& value) {
    if (value.isNull())
        return {};
    require(value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0,
            "Invalid saved timing.");
    return value.toDouble();
}
}
#endif
// Index file identities and sizes only. Decode report bodies when a reader
// compares recent history or when a retention plan needs to validate a file.
history_store::completed_state history_store::inspect(const preferences& prefs) const {
#ifdef CC_PROFILE_HISTORY
    completed_state result{prefs.saving, 0, {}, prefs.retention, 0, {}};
    check_path(folder(), true);
    for (const auto& file : QDir(folder()).entryInfoList(
             QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::NoSort)) {
        const auto name = file.fileName();
        if (name.size() != 49 || !name.startsWith("native-scan-") || !name.endsWith(".json") ||
            !valid_id(name.mid(12, 32)))
            continue;
        result.bytes += file.size();
        result.records.push_back({name, file.size(), file.lastModified().toMSecsSinceEpoch()});
    }
    result.count = static_cast<int>(result.records.size());
    std::ranges::sort(result.records, [](const auto& a, const auto& b) {
        return a.modified == b.modified ? a.name < b.name : a.modified < b.modified;
    });
    return result;
#else
    return {prefs.saving, static_cast<int>(owned(folder()).size())};
#endif
}
#ifdef CC_PROFILE_HISTORY
// Decode a bounded envelope into validated channel observations, recalculating
// summaries instead of trusting stored aggregates. A malformed channel rejects
// the whole record; original bytes are returned only after full validation.
std::vector<history_store::stored_channel_row> history_store::read_rows(
    const record_info& item, QByteArray* original) const {
    const auto file = QDir(folder()).filePath(item.name);
    require(item.bytes > 0 && item.bytes <= record_limit,
            "Unrecognised native history file size; kept.");
    const auto raw = read_bounded(file, record_limit);
    const auto envelope = QJsonDocument::fromJson(raw).object();
    require(raw.size() == item.bytes && envelope["Format"] == namespace_tag &&
                envelope["Id"].toString() == item.name.mid(12, 32) &&
                envelope["MeasurementPolicy"] == "native-deadline-observed-before-cutoff" &&
                envelope["Report"].isString(),
            "Unrecognised native history report; kept.");

    // Establish ownership and schema before allocating per-channel observations.
    const auto doc = QJsonDocument::fromJson(envelope["Report"].toString().toUtf8()).object();
    require(doc["SchemaVersion"].toInt() == 7 && doc["Result"].isObject(),
            "Unknown saved scan schema.");
    const auto scan = doc["Result"].toObject(), opts = scan["Options"].toObject();
    const auto channels = scan["Channels"].toArray();
    const auto count = opts["Samples"].toInt();
    require(channels.size() > 0 && channels.size() <= 30 && count >= 10 && count <= 120,
            "Invalid saved scan bounds.");
    const auto id = envelope["Id"].toString();
    std::vector<stored_channel_row> validated;
    for (const auto& value : channels) {
        const auto channel = value.toObject(), endpoint = channel["Endpoint"].toObject();
        const auto samples = channel["Samples"].toArray();
        require(samples.size() == count, "Saved scan has incomplete rounds.");
        std::vector<cc::observation> observations;
        for (const auto& sample : samples) {
            const auto sample_fields = sample.toObject();
            const auto round = sample_fields["Round"].toInt();
            const auto total = report_timing(sample_fields["LatencyMs"]),
                       connect = report_timing(sample_fields["ConnectMs"]);
            const bool failed = sample_fields["Failure"].isString() &&
                                !sample_fields["Failure"].toString().isEmpty();
            require(round == static_cast<int>(observations.size()) + 1 &&
                        ((failed && !total) || (!failed && total && connect && *total >= *connect)),
                    "Inconsistent saved observation.");
            observations.push_back({round, total, connect, failed});
        }
        // Convert only consistent, ordered rounds into comparison rows. Preserve
        // scan identity and measurement options so incompatible scans stay apart.
        const auto summary = cc::summarize_history_channel(observations);
        cc::history_row row{};
        row.schema = row.revision = row.completed = row.method = row.region = row.host = 1;
        row.channel = endpoint["Channel"].toInt();
        row.port = endpoint["Port"].toInt();
        row.timeout_ms = opts["TimeoutMs"].toInt();
        row.interval_ms = opts["IntervalMs"].toInt();
        row.concurrency = opts["MaxConcurrency"].toInt();
        row.started_ticks = report_ticks(scan["StartedAtUtc"]);
        row.completed_ticks = report_ticks(scan["CompletedAtUtc"]);
        for (int i = 0; i < 4; ++i)
            row.scan_key[static_cast<std::size_t>(i)] = id.mid(i * 8, 8).toUInt(nullptr, 16);
        row.mean_total_ms = summary.total.mean;
        row.variation_ms = summary.total_variation;
        row.successful = summary.successful;
        row.attempted = summary.attempted;
        row.failed = summary.failed;
        const auto key =
            QString("%1,%2,%3").arg(row.channel).arg(endpoint["Host"].toString()).arg(row.port);
        validated.push_back({key, row});
    }
    if (original)
        *original = raw;
    return validated;
}

history_store::retention_plan history_store::preview_limits(limits desired) const {
    const auto guard = open_history_lock(root_, persistent_profile_);
    return plan_limits(load_preferences(), desired, QDateTime::currentMSecsSinceEpoch());
}

// Export publishes its own output atomically, outside the active profile.
// It never persists a preference or authorises deleting a scan.
bool history_store::export_affected(const retention_plan& plan, const QString& output) const {
    if (plan.affected.empty() || output.isEmpty())
        return false;
    const QFileInfo destination(output);
    const auto parent = destination.dir().canonicalPath();
    const auto base = QDir(root_).canonicalPath();
    require(!parent.isEmpty() && parent.compare(base, Qt::CaseInsensitive) != 0 &&
                !parent.startsWith(base + '/', Qt::CaseInsensitive),
            "Choose an export folder outside the application history profile.");
    require(!destination.exists() && !destination.isSymLink() && !destination.isJunction(),
            "Choose a new export file. Existing files will not be overwritten.");
    const auto guard = open_history_lock(root_, persistent_profile_);

    // Revalidate the original receipt under the lock before writing any output.
    // Export preserves original envelopes; it does not grant later deletion.
    const auto latest = plan_limits(load_preferences(), plan.proposed, plan.receipt_ms);
    if (latest != plan)
        return false;
    require(!QFileInfo::exists(output), "An export file already exists at that location.");
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly), "Could not open export destination.");
    const auto write = [&](const QByteArray& chunk) {
        require(file.write(chunk) == chunk.size(),
                "Could not write history export. Original scans were kept.");
    };
    write("{\"Format\":\"MabiTools.Qt.HistoryExport.1\",\"Records\":[");
    bool first = true;
    for (const auto& item : plan.affected) {
        QByteArray record;
        (void)read_rows(item, &record);
        if (!first)
            write(",");
        first = false;
        write(record);
    }
    write("]}");
    require(file.commit(), "Could not publish history export. Original scans were kept.");
    return true;
}

// Recompute the deletion plan while holding the cross-process lock. Consent
// covers this exact plan, not just the requested limits; stale plans remove nothing.
bool history_store::apply_limits(const retention_plan& plan) {
    const auto guard = open_history_lock(root_, persistent_profile_);
    auto prefs = load_preferences();
    // Use the current clock for confirmation as well. Normalise the preview
    // timestamp only for equality; a newly expired record still forces review.
    auto latest = plan_limits(prefs, plan.proposed, QDateTime::currentMSecsSinceEpoch());
    latest.receipt_ms = plan.receipt_ms;
    if (latest != plan)
        return false;
    prefs.retention = plan.proposed;
    store_preferences(prefs); // If writing preferences fails, do not remove anything.

    // Removing multiple existing files cannot be made atomic by QSaveFile.
    // On a partial failure, refresh usage and report it rather than claiming
    // that the requested limit was reached.
    const bool removed = remove_planned(latest);
    // Queue a fresh view under the lock instead of overwriting worker state.
    enqueue([this] {
        const auto hold = open_history_lock(root_, persistent_profile_);
        return inspect(load_preferences());
    });
    if (!removed)
        throw std::runtime_error(
            "Limit saved, but some scans could not be removed. Review the remaining usage.");
    return true;
}

// Copy the recent index under the queue mutex, then decode without holding it.
// Files can disappear during those reads; skip failures without changing storage.
history_replay history_store::compare(const QString& endpoint, const cc::scanning::options& options,
                                      const QDateTime& now, const QDateTime& maintenance) const {
    history_replay model;
    model.endpoint_csv = endpoint;
    cc::history_query query{};
    query.method = query.region = query.host = 1;
    query.timeout_ms = options.timeout_ms;
    query.interval_ms = options.interval_ms;
    query.concurrency = options.max_concurrency;
    query.now_ticks = utc_ticks(now);
    query.seven_days_ago_ticks = query.now_ticks - 7LL * 86400 * 10'000'000;
    if (maintenance.isValid())
        query.maintenance_ticks = utc_ticks(maintenance);
    std::vector<record_info> recent;
    {
        const std::lock_guard guard(queue_gate_);
        const auto earliest =
            std::max(query.seven_days_ago_ticks, query.maintenance_ticks.value_or(0));
        for (const auto& item : records_)
            if (621355968000000000LL + item.modified * 10'000LL >= earliest)
                recent.push_back(item);
    }
    for (const auto& item : recent) {
        try {
            for (const auto& row : read_rows(item)) {
                if (row.endpoint != endpoint)
                    continue;
                query.channel = row.measurement.channel;
                query.port = row.measurement.port;
                model.rows.push_back(row.measurement);
            }
        } catch (const std::runtime_error&) { /* Failed reads never trigger removal. */
        }
    }
    model.summary = cc::compare_history(model.rows, query);
    return model;
}
#endif
std::optional<history_store::ticket> history_store::admit(admission request, std::stop_token stop) {
    // A storage error does not stop a valid probe or erase an exportable report.
    try {
        const auto guard = open_history_lock(root_, persistent_profile_, stop);
        if (!guard.valid() || stop.stop_requested() || request.generation != generation_.load())
            return {};
        const auto prefs = load_preferences();
        if (!prefs.saving || request.requested_ms <= prefs.invalidated_ms)
            return {};
        // Until initialisation commits, a missing file does not grant admission.
        if (!QFile::exists(QDir(root_).filePath("native-history-preferences.json")))
            return {};
        return ticket{request, prefs.epoch, uuid()};
    } catch (const std::exception& reason) {
        const std::lock_guard guard(queue_gate_);
        state_.error =
            QString("Scan was not admitted to scan history. ") + QString::fromUtf8(reason.what());
        return {};
    }
}

// The ticket binds a completed scan to the save setting that admitted it.
// Clear/Off can invalidate it while the measurement is still in flight.
void history_store::enqueue_save(ticket entry, QByteArray report) {
    enqueue([this, entry = std::move(entry), report = std::move(report)] {
        const auto guard = open_history_lock(root_, persistent_profile_);
        const auto prefs = load_preferences();
        // The durable epoch catches changes from other processes; generation
        // catches a local Clear/Off click even before its queued disk job runs.
        const auto valid = [&] { return entry.request.generation == generation_.load(); };
        if (!valid() || !prefs.saving || entry.epoch != prefs.epoch ||
            entry.request.requested_ms <= prefs.invalidated_ms)
            return inspect(prefs);
        require(report.size() < record_limit, "A scan exceeds the per-report bound.");
#ifndef CC_PROFILE_HISTORY
        require(inspect(prefs).count < record_count_limit, "Scan history fixture is full.");
#endif
        const auto schema = QJsonDocument::fromJson(report).object();
        require(schema["SchemaVersion"].toInt() == 7 &&
                    schema["Result"].toObject()["CompletedAtUtc"].isString(),
                "Expected a completed native schema-7 export.");
        const auto data = QJsonDocument(QJsonObject{{"Format", namespace_tag},
                                                    {"Id", entry.id},
                                                    {"MeasurementPolicy",
                                                     "native-deadline-observed-before-cutoff"},
                                                    {"Report", QString::fromUtf8(report)}})
                              .toJson(QJsonDocument::Compact);
        publish_record(folder(), QDir(folder()).filePath("native-scan-" + entry.id + ".json"), data,
                       valid);
#ifdef CC_PROFILE_HISTORY
        // Enforce the already agreed limits after publication, under the same
        // profile lock. Recheck local admission before planning and removing.
        bool pruned = true;
        if (valid()) {
            const auto plan =
                plan_limits(prefs, prefs.retention, QDateTime::currentMSecsSinceEpoch());
            if (valid())
                pruned = remove_planned(plan);
        }
        auto result = inspect(prefs);
        if (!pruned)
            result.warning = "Some old scans could not be removed. The remaining usage is shown.";
        return result;
#else
        return inspect(prefs);
#endif
    });
}

void history_store::set_saving(bool value) {
    if (!value)
        ++generation_; // At the click, before queue or lock wait.
    const auto requested = QDateTime::currentMSecsSinceEpoch();
    enqueue([this, value, requested] {
        const auto guard = open_history_lock(root_, persistent_profile_);
        auto prefs = load_preferences();
        prefs.saving = value;
        if (!value) {
            prefs.epoch = uuid();
            prefs.invalidated_ms = std::max(prefs.invalidated_ms, requested);
        }
        store_preferences(prefs);
        return inspect(prefs);
    });
}

// Invalidate local admissions immediately, then persist cross-process invalidation
// under the profile lock before deleting any recognised records.
void history_store::clear() {
    ++generation_;
    const auto requested = QDateTime::currentMSecsSinceEpoch();
    enqueue([this, requested] {
        const auto guard = open_history_lock(root_, persistent_profile_);
        auto prefs = load_preferences();
        prefs.epoch = uuid();
        prefs.invalidated_ms = std::max(prefs.invalidated_ms, requested);
        store_preferences(prefs); // Failed durable invalidation must not delete history.
        for (const auto& path : owned(folder()))
            require(QFile::remove(path), "Could not remove an owned scan history file.");
        return inspect(prefs);
    });
}

void history_store::enqueue(std::function<completed_state()> work) {
    {
        const std::lock_guard guard(queue_gate_);
        if (shutdown_)
            return;
        queue_.push_back(std::move(work));
        ++state_.pending;
    }
    wake_.notify_one();
}

history_store::status history_store::view() const {
    const std::lock_guard guard(queue_gate_);
    return state_;
}

// Process jobs in FIFO order. queue_gate_ protects only queue/status/index state;
// disk jobs take the profile lock themselves, without holding queue_gate_.
// Shutdown stops admission but drains jobs already accepted before exiting.
void history_store::run() {
    for (;;) {
        std::function<completed_state()> work;
        {
            std::unique_lock guard(queue_gate_);
            wake_.wait(guard, [&] { return shutdown_ || !queue_.empty(); });
            if (queue_.empty())
                return;
            work = std::move(queue_.front());
            queue_.pop_front();
        }

        // Release the queue mutex before disk I/O or a profile-lock wait so UI
        // status reads and new requests do not wait for another process.
        std::optional<completed_state> result;
        QString error;
        try {
            result = work();
        } catch (const std::exception& reason) {
            error = QString::fromUtf8(reason.what());
        }

        // Publish one completed view atomically. Failed jobs preserve the last
        // successful view and surface their error instead of stopping the queue.
        const std::lock_guard guard(queue_gate_);
        if (result) {
            state_.loaded = true;
            state_.saving = result->saving;
            state_.count = result->count;
#ifdef CC_PROFILE_HISTORY
            records_ = std::move(result->records);
            state_.retention = result->retention;
            state_.bytes = result->bytes;
            if (error.isEmpty())
                error = result->warning;
            if (state_.bytes > static_cast<qint64>(state_.retention.mib) * 1024 * 1024 &&
                error.isEmpty())
                error =
                    "History is above its limit. Some files may be unreadable or could not be removed.";
#endif
        }
        state_.error = error;
        --state_.pending;
    }
}

void history_store::drain() {
    {
        const std::lock_guard guard(queue_gate_);
        shutdown_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable())
        worker_.join();
}
}
