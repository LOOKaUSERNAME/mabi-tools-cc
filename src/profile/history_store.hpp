#pragma once
#include <QByteArray>
#ifdef CC_PROFILE_HISTORY
#include "history_replay.hpp"
#include "cc_scan.hpp"
#endif
#include <QString>
#include <QDateTime>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace cc::ui {
// Owns the native Qt profile's immutable scans and save preferences.
// WPF archives are a separate read-only store; this class never opens them.
// The worker serialises its queue, and the profile lock coordinates separate
// processes that open the same native history.
class history_store final {
public:
    // A scan captures its generation and time before the worker is started.
    // The ticket also records the durable epoch, so Clear or Off in another
    // process can invalidate an in-flight save.
    struct admission {
        std::uint64_t generation{};
        qint64 requested_ms{};
    };

    struct ticket {
        admission request;
        QString epoch;
        QString id;
    };

    struct limits {
        // days==0 disables the age limit; mib is a byte budget in 1024^2 bytes.
        int days{90}, mib{100};
        bool operator==(const limits&) const = default;
    };

    struct status {
        bool loaded{};
        bool saving{};
        int count{};
        int pending{};
        QString error;
        limits retention;
        qint64 bytes{};
    };
#ifdef CC_PROFILE_HISTORY
    struct record_info {
        QString name;
        qint64 bytes{}, modified{};
        bool operator==(const record_info&) const = default;
    };

    struct retention_plan {
        limits previous, proposed;
        std::vector<record_info> affected;
        qint64 bytes{}, receipt_ms{};
        bool over_budget{};
        bool operator==(const retention_plan&) const = default;
    };

    // Preview is read-only. Export/apply revalidate file identities and limits
    // under the profile lock; false means consent is stale and needs a new plan.
    // Export copies originals outside the profile without authorising deletion.
    // Storage errors may throw. Deletion is not transactional: partial removal
    // failures are reported with actual remaining usage, never a false rollback.
    retention_plan preview_limits(limits desired) const;
    bool export_affected(const retention_plan&, const QString& output) const;
    bool apply_limits(const retention_plan&);
#endif

    explicit history_store(const QString& root, bool persistent_profile = false);
    ~history_store();
    history_store(const history_store&) = delete;
    history_store& operator=(const history_store&) = delete;
    // Capture on scan request, then admit on its worker before probing. admit
    // may wait for the profile lock and returns no ticket when saving is off,
    // invalidated or unavailable; a storage failure must not prevent measurement.
    admission capture() const;
    std::optional<ticket> admit(admission request, std::stop_token stop);
    // Enqueue complete report bytes only. Clear/Off invalidates admission before
    // queued disk work, including saves from another process via a durable epoch.
    // The worker rechecks the ticket before publishing an immutable scan file.
    void enqueue_save(ticket entry, QByteArray report);
    void set_saving(bool value);
    void clear();
    // Thread-safe status snapshot; asynchronous I/O failures appear in error.
    status view() const;
    QString folder() const;
#ifdef CC_PROFILE_HISTORY
    history_replay compare(const QString& endpoint, const cc::scanning::options&,
                           const QDateTime& now, const QDateTime& maintenance = {}) const;
#endif
    // Stop accepting work, finish queued jobs and join. Call before destroying
    // consumers of captured state; this can block and does not cancel disk jobs.
    void drain();

private:
    struct preferences {
        bool saving{true};
        qint64 invalidated_ms{-1};
        QString epoch;
        limits retention{};
    };
#ifdef CC_PROFILE_HISTORY
    struct stored_channel_row {
        QString endpoint;
        cc::history_row measurement;
    };
#endif
    struct completed_state {
        bool saving;
        int count;
#ifdef CC_PROFILE_HISTORY
        std::vector<record_info> records;
        limits retention{};
        qint64 bytes{};
        QString warning;
#endif
    };

    QString root_;
    bool persistent_profile_{};
    std::atomic<std::uint64_t> generation_{};
    mutable std::mutex queue_gate_;
    std::condition_variable wake_;
    std::deque<std::function<completed_state()>> queue_;
    status state_;
#ifdef CC_PROFILE_HISTORY
    std::vector<record_info> records_;
    retention_plan plan_limits(const preferences&, limits desired, qint64 now) const;
    std::vector<stored_channel_row> read_rows(const record_info&,
                                              QByteArray* original = nullptr) const;
    bool remove_planned(const retention_plan&) const;
#endif
    bool shutdown_{};
    std::jthread worker_;
    void enqueue(std::function<completed_state()> work);
    void run();
    preferences load_preferences() const;
    void store_preferences(const preferences& prefs) const;
    completed_state inspect(const preferences& prefs) const;
};
}
