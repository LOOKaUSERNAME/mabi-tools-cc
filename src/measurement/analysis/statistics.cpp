// Pure scan analysis: validate and order observations, derive per-channel
// statistics, then compare eligible channels over the full scan and both halves.
// Network timing and presentation are callers' responsibilities; evaluation
// order here also preserves report compatibility at floating-point boundaries.

#include "cc_core.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cc {
namespace {
std::vector<observation> ordered(std::span<const observation> samples) {
    std::vector<observation> result(samples.begin(), samples.end());
    std::stable_sort(result.begin(), result.end(),
                     [](const auto& a, const auto& b) { return a.round < b.round; });
    return result;
}

std::optional<double> mean(std::span<const double> values) {
    if (values.empty())
        return std::nullopt;
    double total = 0;
    for (const double value : values)
        total += value;
    return total / static_cast<double>(values.size());
}
}

statistics calculate(std::span<const observation> samples) {
    const auto rounds = ordered(samples);
    if (rounds.size() > 120)
        throw std::invalid_argument("At most 120 observations are supported.");
    std::int32_t previous_round = 0;
    for (const auto& sample : rounds) {
        if (sample.round < 1 || sample.round > 120 || sample.round == previous_round)
            throw std::invalid_argument("Rounds must be unique and between 1 and 120.");
        previous_round = sample.round;
        for (const auto duration : {sample.total_ms, sample.connection_ms})
            if (duration && (!std::isfinite(*duration) || *duration < 0))
                throw std::invalid_argument("Durations must be finite, non-negative milliseconds.");
        if (sample.total_ms.has_value() == sample.has_failure)
            throw std::invalid_argument("Each observation needs either a total or a failure.");
        if (sample.total_ms && (!sample.connection_ms || *sample.connection_ms > *sample.total_ms))
            throw std::invalid_argument(
                "A complete reply needs a connection time no greater than its total.");
    }

    // Match the reference's evaluation order: connection means use sorted
    // successful values; reply/total means use round order. No failed reply
    // contributes its possibly recorded connection duration to these means.
    std::vector<double> connections, replies, totals, connection_changes, total_changes;
    for (std::size_t index = 0; index < rounds.size(); ++index) {
        const auto& sample = rounds[index];
        if (sample.total_ms) {
            connections.push_back(*sample.connection_ms);
            replies.push_back(*sample.total_ms - *sample.connection_ms);
            totals.push_back(*sample.total_ms);
        }
        if (index == 0)
            continue;
        const auto& previous = rounds[index - 1];
        if (sample.round == previous.round + 1 && sample.total_ms && previous.total_ms) {
            connection_changes.push_back(std::abs(*sample.connection_ms - *previous.connection_ms));
            total_changes.push_back(std::abs(*sample.total_ms - *previous.total_ms));
        }
    }
    std::sort(connections.begin(), connections.end());
    statistics result;
    result.attempted = static_cast<std::int32_t>(rounds.size());
    result.successful = static_cast<std::int32_t>(connections.size());
    result.failed = result.attempted - result.successful;
    result.adjacent_pairs = static_cast<std::int32_t>(connection_changes.size());
    result.success_rate =
        result.attempted == 0 ? 0 : static_cast<double>(result.successful) / result.attempted;
    result.average_connection_ms = mean(connections);
    if (!connections.empty()) {
        result.minimum_connection_ms = connections.front();
        result.maximum_connection_ms = connections.back();
        result.p95_connection_ms =
            connections[static_cast<std::size_t>(std::ceil(connections.size() * 0.95)) - 1];
    }
    result.connection_jitter_ms = mean(connection_changes);
    result.average_reply_ms = mean(replies);
    result.average_total_ms = mean(totals);
    result.total_jitter_ms = mean(total_changes);
    if (result.average_total_ms && result.total_jitter_ms)
        result.score_ms = *result.average_total_ms + 2 * *result.total_jitter_ms;
    result.status = result.attempted == 0    ? sample_status::not_sampled
                    : result.successful == 0 ? sample_status::no_response
                    : result.successful < 5 || result.adjacent_pairs < 3
                        ? sample_status::insufficient
                    : result.success_rate < 0.8 ? sample_status::unreliable
                                                : sample_status::eligible;
    return result;
}

namespace {
struct candidate {
    std::int32_t index, number;
    statistics full, early, late;
    std::vector<std::int32_t> rounds;
};
enum class metric { connection, consistency, overall };

double value(const statistics& stats, metric measurement) {
    if (measurement == metric::connection)
        return stats.average_connection_ms.value();
    if (measurement == metric::consistency)
        return stats.total_jitter_ms.value();
    return stats.score_ms.value();
}

bool clearly_better(const statistics& chosen, const statistics& other, metric measurement) {
    if (measurement == metric::overall) {
        if (chosen.success_rate < other.success_rate)
            return false;
        if (chosen.success_rate - other.success_rate >= 0.10 - 1e-9)
            return true;
    }
    const double reference = value(other, measurement);
    const double margin = measurement == metric::consistency ? std::max(1.0, reference * 0.10)
                                                             : std::max(2.0, reference * 0.05);
    return reference - value(chosen, measurement) >= margin;
}
}

scan_summary summarize(std::span<const channel> channels) {
    scan_summary result;
    for (const auto& input : channels)
        result.channels.push_back(calculate(input.samples));
    result.picks = select(channels, result.channels);
    return result;
}

recommendations select(std::span<const channel> channels, std::span<const statistics> summaries) {
    if (channels.size() != summaries.size())
        throw std::invalid_argument("Channel summary count mismatch.");
    recommendations result;
    // Require evidence in both halves before ranking: a good full-scan mean
    // alone can conceal deterioration or an unreliable part of the scan.
    std::vector<candidate> candidates;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const auto& input = channels[index];
        const auto full = summaries[index];
        if (!full.eligible() || input.samples.size() < 10)
            continue;
        const auto sorted = ordered(input.samples);
        const auto split = sorted.size() / 2; // The late half gets the extra observation.
        const auto early = calculate(std::span(sorted).first(split));
        const auto late = calculate(std::span(sorted).subspan(split));
        if (!early.eligible() || !late.eligible())
            continue;
        candidate item{static_cast<std::int32_t>(index), input.number, full, early, late, {}};
        for (const auto& sample : sorted)
            item.rounds.push_back(sample.round);
        candidates.push_back(std::move(item));
    }
    if (candidates.size() < 2)
        return result;
    for (const auto& item : candidates)
        if (item.rounds != candidates.front().rounds)
            return result;

    // A practical measured pick always exists once the comparison qualifies.
    // Channel number breaks exact ties; stable input order resolves duplicate IDs.
    const auto choose = [&](metric measurement) -> const candidate& {
        return *std::min_element(
            candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
                if (measurement == metric::overall && a.full.success_rate != b.full.success_rate)
                    return a.full.success_rate > b.full.success_rate;
                const double av = value(a.full, measurement), bv = value(b.full, measurement);
                return av != bv ? av < bv : a.number < b.number;
            });
    };
    // Ranking and confidence are separate. A marginal winner remains useful,
    // but only a lead that survives every window receives clear-lead wording.
    const auto lead = [&](const candidate& chosen, metric measurement) {
        for (const auto& other : candidates) {
            if (&other == &chosen)
                continue;
            if (!clearly_better(chosen.full, other.full, measurement) ||
                !clearly_better(chosen.early, other.early, measurement) ||
                !clearly_better(chosen.late, other.late, measurement))
                return recommendation_status::best_measured;
        }
        return recommendation_status::clear_lead;
    };
    const auto& fastest = choose(metric::connection);
    const auto& consistent = choose(metric::consistency);
    const auto& overall = choose(metric::overall);
    result = {fastest.index,
              consistent.index,
              overall.index,
              lead(fastest, metric::connection),
              lead(consistent, metric::consistency),
              lead(overall, metric::overall),
              false};
    for (const auto& other : candidates) {
        if (&other == &overall)
            continue;
        if (overall.full.success_rate - other.full.success_rate < 0.10 - 1e-9 &&
            std::abs(*overall.full.score_ms - *other.full.score_ms) <
                std::max(2.0, *other.full.score_ms * 0.05))
            result.overall_results_similar = true;
    }
    return result;
}

const char* status_text(sample_status status) {
    switch (status) {
    case sample_status::not_sampled:
        return "Not sampled";
    case sample_status::no_response:
        return "No response";
    case sample_status::insufficient:
        return "Insufficient samples";
    case sample_status::unreliable:
        return "Unreliable";
    case sample_status::eligible:
        return "Eligible";
    }
    throw std::invalid_argument("Unknown sample status.");
}
}
