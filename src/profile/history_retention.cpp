// Chooses and removes eligible scan files. Callers hold the profile lock.
#include "history_store.hpp"
#include <QDir>
#include <QFile>
#include <stdexcept>
#include <utility>

#ifdef CC_PROFILE_HISTORY
namespace cc::ui {
namespace {
constexpr qint64 bytes_per_mib = 1024 * 1024;
constexpr qint64 milliseconds_per_day = 86'400'000;
constexpr qint64 ticks_per_millisecond = 10'000;
constexpr qint64 unix_epoch_ticks = 621'355'968'000'000'000;
}

// Build a read-only removal proposal from the oldest-first index. Validate each
// selected report before treating it as removable, then check whether protected
// files make the byte cap unattainable. The caller holds the profile lock while
// this function inspects files; applying a later confirmation requires replanning.
history_store::retention_plan history_store::plan_limits(const preferences& prefs, limits desired,
                                                         qint64 now) const {
    const bool valid_age =
        desired.days == 0 || desired.days == 7 || desired.days == 30 || desired.days == 90;
    const bool valid_budget = desired.mib == 100 || desired.mib == 250 || desired.mib == 500;
    if (!valid_age || !valid_budget)
        throw std::runtime_error("Invalid history limits.");
    const auto current = inspect(prefs);
    retention_plan plan{prefs.retention, desired, {}, 0, now, false};
    const auto budget = static_cast<qint64>(desired.mib) * bytes_per_mib;
    const auto expiry_ms = now - static_cast<qint64>(desired.days) * milliseconds_per_day;
    const auto expiry_ticks = unix_epoch_ticks + expiry_ms * ticks_per_millisecond;
    std::vector<record_info> expired_records;
    qint64 expired_bytes = 0;
    // Age and byte pressure are independent reasons to select a valid record.
    // Keep an age-only list as well, for the unattainable-budget fallback below.
    for (const auto& item : current.records) {
        const bool byte_pressure = current.bytes - plan.bytes > budget;
        const bool age_candidate = desired.days != 0 && item.modified < expiry_ms;
        if (!byte_pressure && !age_candidate)
            break; // Remaining metadata is sorted oldest first.
        try {
            const auto rows = read_rows(item);
            // File time is a cheap prefilter. Never age-delete a scan whose
            // recorded completion time is still within the chosen window.
            const bool expired = age_candidate && !rows.empty() &&
                                 rows.front().measurement.completed_ticks < expiry_ticks;
            if (expired) {
                expired_records.push_back(item);
                expired_bytes += item.bytes;
            }
            if (byte_pressure || expired) {
                plan.affected.push_back(item);
                plan.bytes += item.bytes;
            }
        } catch (const std::runtime_error&) {
            // Incompatible or unreadable files stay where they are.
        }
    }
    plan.over_budget = current.bytes - plan.bytes > budget;
    if (plan.over_budget) {
        // Protected bytes alone can prevent reaching the cap. Do not erase
        // every usable scan in a futile attempt to meet it.
        plan.affected = std::move(expired_records);
        plan.bytes = expired_bytes;
    }
    return plan;
}

// The caller has validated the plan under the profile lock. Try every removal
// even after one failure; the caller refreshes usage rather than claiming rollback.
bool history_store::remove_planned(const retention_plan& plan) const {
    bool all_removed = true;
    for (const auto& item : plan.affected) {
        if (!QFile::remove(QDir(folder()).filePath(item.name)))
            all_removed = false;
    }
    return all_removed;
}
} // namespace cc::ui
#endif
