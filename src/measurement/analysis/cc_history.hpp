#pragma once
#include "cc_core.hpp"
#include <array>

namespace cc {
struct timing_summary {
    std::optional<double> mean, minimum, maximum;
};

struct history_channel_summary {
    std::int32_t attempted{}, successful{}, failed{};
    timing_summary connection, reply, total;
    std::optional<double> total_variation;
    std::int32_t adjacent_pairs{};
};

// History accepts older observations with a missing connection duration. Do not
// route these through the stricter current-scan validation in calculate().
history_channel_summary summarize_history_channel(std::span<const observation> samples);

// One row per saved channel, retaining the original flattened input index. String
// identities are ordinal interned tokens shared with the query for this call only.
// The caller provides matching identity tokens; this layer compares those tokens
// and never opens or modifies history files.
struct history_row {
    std::int32_t schema{}, revision{}, completed{}, method{}, region{}, host{}, channel{}, port{};
    std::int32_t timeout_ms{}, interval_ms{}, concurrency{};
    std::int64_t started_ticks{}, completed_ticks{}; // UTC .NET ticks (100 ns).
    std::array<std::uint32_t, 4> scan_key{};         // Guid "N" text, four big-endian words.
    std::optional<double> mean_total_ms, variation_ms;
    std::int32_t successful{}, attempted{}, failed{};
};

struct history_query {
    std::int32_t method{}, region{}, host{}, channel{}, port{}, timeout_ms{}, interval_ms{},
        concurrency{};
    std::int64_t now_ticks{}, seven_days_ago_ticks{};
    std::optional<std::int64_t> maintenance_ticks, reset_ticks;
};
enum class history_period : std::int32_t { seven_days, maintenance, reset };

struct history_summary {
    history_period period{history_period::seven_days};
    std::int64_t cutoff_ticks{};
    std::vector<std::int32_t> indices;
    std::int32_t timing_scans{}, variation_scans{}, first_started_index{-1};
    std::optional<double> typical_total_ms, typical_variation_ms;
    std::int64_t successful{}, attempted{}, failed{};
};

// Selects complete schema/revision-compatible rows with matching query identity
// and options. The cutoff is the latest valid seven-day, maintenance or reset
// boundary; future maintenance/reset values are ignored. Equal reset wins.
// Results contain input indices ordered by completion time then scan key.
// Typical timings are medians of scan means; check counts are summed. The caller
// supplies UTC .NET ticks and already-decoded rows; no files are read or changed.
history_summary compare_history(std::span<const history_row> rows, const history_query& query);
}
