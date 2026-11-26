#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cc {
// Qt-free measurement analysis. Inputs and results are owned values; no I/O,
// clocks or shared state are used.
// All durations are milliseconds. A missing duration is distinct from zero.
// Only the presence of non-whitespace failure text is needed by this
// deterministic calculation; the caller retains the full failure text.
struct observation {
    std::int32_t round;
    std::optional<double> total_ms;
    std::optional<double> connection_ms;
    bool has_failure;
};

struct channel {
    std::int32_t number;
    std::vector<observation> samples;
};
enum class sample_status : std::int32_t {
    not_sampled,
    no_response,
    insufficient,
    unreliable,
    eligible
};
enum class recommendation_status : std::int32_t {
    clear_lead,
    legacy_no_winner,
    not_enough_data,
    best_measured
};

struct statistics {
    std::int32_t attempted{}, successful{}, failed{}, adjacent_pairs{};
    double success_rate{};
    std::optional<double> average_connection_ms, minimum_connection_ms, maximum_connection_ms;
    std::optional<double> p95_connection_ms, connection_jitter_ms;
    std::optional<double> average_reply_ms, average_total_ms, total_jitter_ms, score_ms;
    sample_status status{sample_status::not_sampled};

    bool eligible() const { return status == sample_status::eligible; }
};

struct recommendations {
    // Input indices, not channel numbers: this preserves object identity/order.
    std::int32_t fastest{-1}, consistent{-1}, overall{-1};
    recommendation_status fastest_status{recommendation_status::not_enough_data};
    recommendation_status consistent_status{recommendation_status::not_enough_data};
    recommendation_status overall_status{recommendation_status::not_enough_data};
    bool overall_results_similar{};
};

struct scan_summary {
    std::vector<statistics> channels;
    recommendations picks;
};

// Accepts 0..120 unique rounds in 1..120, in any order. Throws invalid_argument
// for invalid observations. Durations must be finite and non-negative. Success
// requires total >= connection and no failure; failure requires no total but
// may retain a connection duration. Failed replies do not enter timing means.
// Jitter is the mean absolute change between adjacent successful round numbers,
// never across a gap. P95 uses nearest rank; score = mean total + 2 * total jitter.
// Eligibility requires >=5 successes, >=3 adjacent pairs and >=80% success.
statistics calculate(std::span<const observation> samples);
// Calculates each channel before selection; results preserve input order.
scan_summary summarize(std::span<const channel> channels);
// Saved snapshots may contain historical or ineligible observations. Honor their
// supplied full-scan eligibility and summaries; recalculate only the halves of
// candidates when checking whether a lead holds across the scan. The arrays must
// correspond by index (count mismatch throws invalid_argument). A comparison
// needs at least two eligible channels with the same round numbers and eligible
// early/late halves. Invalid observations in those halves also throw.
// Ranking prefers lower connection mean / total jitter; overall prefers success
// rate, then lower score. A clear lead must beat every rival in all three windows.
// Otherwise a best-measured pick remains; absent picks have index -1.
recommendations select(std::span<const channel> channels, std::span<const statistics> full);
const char* status_text(sample_status status);
}
