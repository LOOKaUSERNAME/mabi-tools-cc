// Compare caller-supplied saved observations without accessing storage. History
// tolerates older missing connection timings, filters compatible scan rows, and
// combines per-scan means with medians rather than pooling all latency samples.

#include "cc_history.hpp"
#include <algorithm>
#include <cmath>

namespace cc {
namespace {
timing_summary timing(const std::vector<double>& values) {
    if (values.empty())
        return {};
    double sum = 0;
    for (double value : values)
        sum += value;
    const auto extremes = std::minmax_element(values.begin(), values.end());
    return {sum / static_cast<double>(values.size()), *extremes.first, *extremes.second};
}

void available(std::vector<double>& values, std::optional<double> value) {
    if (value && std::isfinite(*value) && *value >= 0)
        values.push_back(*value);
}

std::optional<double> median(std::vector<double> values) {
    if (values.empty())
        return std::nullopt;
    // Match Double.CompareTo for direct callers as well as validated files. Plain
    // floating-point < is not a strict weak ordering when a value is NaN.
    std::sort(values.begin(), values.end(), [](double a, double b) {
        return std::isnan(a) ? !std::isnan(b) : !std::isnan(b) && a < b;
    });
    const auto middle = values.size() / 2;
    // Divide before adding, matching C# and avoiding overflow for large medians.
    return values.size() % 2 ? values[middle] : values[middle - 1] / 2 + values[middle] / 2;
}
}

history_channel_summary summarize_history_channel(std::span<const observation> samples) {
    std::vector<observation> rounds(samples.begin(), samples.end());
    std::stable_sort(rounds.begin(), rounds.end(),
                     [](const auto& a, const auto& b) { return a.round < b.round; });
    std::vector<double> connections, replies, totals, differences;
    history_channel_summary result;
    result.attempted = static_cast<std::int32_t>(rounds.size());
    for (std::size_t index = 0; index < rounds.size(); ++index) {
        const auto& sample = rounds[index];
        if (sample.total_ms && !sample.has_failure) {
            ++result.successful;
            available(connections, sample.connection_ms);
            available(replies, sample.connection_ms
                                   ? std::optional(*sample.total_ms - *sample.connection_ms)
                                   : std::nullopt);
            available(totals, sample.total_ms);
        }
        if (index == 0)
            continue;
        const auto& previous = rounds[index - 1];
        if (static_cast<std::int64_t>(sample.round) ==
                static_cast<std::int64_t>(previous.round) + 1 &&
            sample.total_ms && previous.total_ms && !sample.has_failure && !previous.has_failure)
            differences.push_back(std::abs(*sample.total_ms - *previous.total_ms));
    }
    result.failed = result.attempted - result.successful;
    result.connection = timing(connections);
    result.reply = timing(replies);
    result.total = timing(totals);
    result.total_variation = timing(differences).mean;
    result.adjacent_pairs = static_cast<std::int32_t>(differences.size());
    return result;
}

history_summary compare_history(std::span<const history_row> rows, const history_query& query) {
    history_summary result;
    result.cutoff_ticks = query.seven_days_ago_ticks;
    if (query.maintenance_ticks && *query.maintenance_ticks <= query.now_ticks &&
        *query.maintenance_ticks > result.cutoff_ticks) {
        result.cutoff_ticks = *query.maintenance_ticks;
        result.period = history_period::maintenance;
    }
    // A reset wins an equal cutoff; a maintenance event must be strictly newer.
    if (query.reset_ticks && *query.reset_ticks <= query.now_ticks &&
        *query.reset_ticks >= result.cutoff_ticks) {
        result.cutoff_ticks = *query.reset_ticks;
        result.period = history_period::reset;
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        if (!row.completed || row.schema != 1 || row.revision != 1 || row.method != query.method ||
            row.started_ticks < result.cutoff_ticks || row.completed_ticks > query.now_ticks ||
            row.completed_ticks < row.started_ticks || row.timeout_ms != query.timeout_ms ||
            row.interval_ms != query.interval_ms || row.concurrency != query.concurrency ||
            row.region != query.region || row.host != query.host || row.channel != query.channel ||
            row.port != query.port)
            continue;
        result.indices.push_back(static_cast<std::int32_t>(index));
    }
    // Equal timestamps use Guid.CompareTo order. Canonical N-format words have
    // the same unsigned component ordering; stable sort retains duplicate rows.
    std::stable_sort(result.indices.begin(), result.indices.end(), [&](auto a, auto b) {
        if (rows[a].completed_ticks != rows[b].completed_ticks)
            return rows[a].completed_ticks < rows[b].completed_ticks;
        return rows[a].scan_key < rows[b].scan_key;
    });
    // Aggregate only eligible rows. Missing timing/variation is not zero: each
    // median has its own contributing count, while attempt totals include failures.
    std::vector<double> totals, variations;
    for (const auto index : result.indices) {
        const auto& row = rows[index];
        if (row.mean_total_ms)
            totals.push_back(*row.mean_total_ms);
        if (row.variation_ms)
            variations.push_back(*row.variation_ms);
        result.successful += row.successful;
        result.attempted += row.attempted;
        result.failed += row.failed;
        if (result.first_started_index < 0 ||
            row.started_ticks < rows[result.first_started_index].started_ticks)
            result.first_started_index = index;
    }
    result.timing_scans = static_cast<std::int32_t>(totals.size());
    result.variation_scans = static_cast<std::int32_t>(variations.size());
    result.typical_total_ms = median(totals);
    result.typical_variation_ms = median(variations);
    return result;
}
}
