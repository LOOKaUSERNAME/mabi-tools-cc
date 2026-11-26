// Schema-7 report formatting and JSON/UTC helpers. This layer returns bytes;
// callers supply build identity and own persistence, as described in cc_report.hpp.

#include "cc_report.hpp"
#include "report_text.hpp"
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace cc::scanning {
namespace {
void string(std::ostream& out, const std::string& value) {
    out << '"';
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\')
            out << '\\' << static_cast<char>(ch);
        else if (ch < 32)
            out << "\\u00" << "0123456789abcdef"[ch >> 4] << "0123456789abcdef"[ch & 15];
        else
            out << static_cast<char>(
                ch); // UTF-8 is valid JSON, including the ranking's multiplication sign.
    }
    out << '"';
}

void number(std::ostream& out, std::optional<double> value) {
    if (!value)
        out << "null";
    else {
        if (!std::isfinite(*value))
            throw std::invalid_argument("Non-finite report value");
        out << *value;
    }
}

void sample_json(std::ostream& out, const measurement& sample) {
    out << "{\"Round\":" << sample.round << ",\"LatencyMs\":";
    number(out, sample.total);
    out << ",\"ConnectMs\":";
    number(out, sample.connection);
    out << ",\"Failure\":";
    if (sample.failure.empty())
        out << "null";
    else
        string(out, sample.failure);
    out << ",\"Timing\":{\"StartOffsetMs\":";
    number(out, sample.start_offset);
    out << ",\"ConnectOffsetMs\":";
    number(out, sample.connection ? std::optional(sample.start_offset + *sample.connection)
                                  : std::nullopt);
    out << ",\"TerminalOffsetMs\":";
    number(out, sample.terminal_offset);
    out << ",\"ElapsedMs\":";
    number(out, sample.elapsed);
    out << ",\"Stage\":";
    string(out, sample.stage);
    out << ",\"Outcome\":";
    string(out, sample.outcome);
    out << ",\"SocketError\":";
    if (sample.socket_error)
        string(out, socket_name(sample.socket_error));
    else
        out << "null";
    out << ",\"NativeSocketError\":";
    if (sample.socket_error)
        out << sample.socket_error;
    else
        out << "null";
    // PingMs is the legacy exported successful-connection property, not a new
    // measurement. A failed reply excludes the connection from this property.
    out << "},\"PingMs\":";
    number(out, sample.total ? sample.connection : std::nullopt);
    out << ",\"ReplyWaitMs\":";
    number(out, sample.total && sample.connection
                    ? std::optional(*sample.total - *sample.connection)
                    : std::nullopt);
    out << '}';
}

void channel_json(std::ostream& out, const channel_measurements& channel, const cc::statistics& s) {
    out << "{\"Endpoint\":{\"Channel\":" << channel.target.number << ",\"Host\":";
    string(out, channel.target.host);
    out << ",\"Port\":" << channel.target.port << "},\"Samples\":[";
    for (std::size_t i = 0; i < channel.samples.size(); ++i) {
        if (i)
            out << ',';
        sample_json(out, channel.samples[i]);
    }
    out << "],\"Attempted\":" << s.attempted << ",\"Successful\":" << s.successful
        << ",\"Failed\":" << s.failed;
    out << ",\"SuccessRate\":";
    number(out, s.success_rate);
    out << ",\"AverageMs\":";
    number(out, s.average_connection_ms);
    out << ",\"MinimumMs\":";
    number(out, s.minimum_connection_ms);
    out << ",\"MaximumMs\":";
    number(out, s.maximum_connection_ms);
    out << ",\"P95Ms\":";
    number(out, s.p95_connection_ms);
    out << ",\"JitterMs\":";
    number(out, s.connection_jitter_ms);
    out << ",\"AdjacentPairs\":" << s.adjacent_pairs
        << ",\"IsEligible\":" << (s.eligible() ? "true" : "false") << ",\"Status\":";
    string(out, cc::status_text(s.status));
    out << ",\"ScoreMs\":";
    number(out, s.score_ms);
    out << ",\"AverageReplyWaitMs\":";
    number(out, s.average_reply_ms);
    out << ",\"AverageResponseMs\":";
    number(out, s.average_total_ms);
    out << ",\"ResponseJitterMs\":";
    number(out, s.total_jitter_ms);
    out << '}';
}

const char* status(cc::recommendation_status value) {
    switch (value) {
    case cc::recommendation_status::clear_lead:
        return "ClearLead";
    case cc::recommendation_status::best_measured:
        return "BestMeasured";
    case cc::recommendation_status::legacy_no_winner:
        return "NoClearWinner";
    default:
        return "NotEnoughData";
    }
}
}

std::string utc_text(std::chrono::system_clock::time_point time) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(time);
    const auto ticks =
        std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
            time - seconds)
            .count();
    const auto unix_time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (gmtime_s(&utc, &unix_time))
        throw std::invalid_argument("Invalid report UTC time");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (ticks) {
        auto fraction = std::to_string(10000000 + ticks).substr(1);
        while (fraction.back() == '0')
            fraction.pop_back();
        out << '.' << fraction;
    }
    out << "+00:00";
    return out.str();
}

// Validate completeness and reconstruct numerical inputs before writing JSON.
// Recompute summaries from observations rather than trusting the snapshot's
// cached picks; caller-supplied metadata remains the report's build identity.
std::string report_json(const result& scan, options settings, const report_metadata& metadata) {
    if (!scan.values || scan.completed_utc < scan.started_utc)
        throw std::invalid_argument("Invalid completed scan");
    std::vector<endpoint> targets;
    std::vector<cc::channel> observations;
    for (const auto& channel : scan.values->channels) {
        targets.push_back(channel.target);
        if (channel.samples.size() != static_cast<std::size_t>(settings.samples))
            throw std::invalid_argument("Cannot export an incomplete scan");
        cc::channel input{channel.target.number, {}};
        for (std::size_t i = 0; i < channel.samples.size(); ++i) {
            const auto& sample = channel.samples[i];
            if (sample.round != static_cast<int>(i) + 1)
                throw std::invalid_argument("Incomplete or unordered measured rounds");
            input.samples.push_back(
                {sample.round, sample.total, sample.connection, !sample.failure.empty()});
        }
        observations.push_back(std::move(input));
    }
    validate(targets, settings);
    const auto summary = cc::summarize(observations);
    // Locale-independent decimal points and enough digits to round-trip doubles
    // preserve report values regardless of the player's Windows regional settings.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "{\"SchemaVersion\":7,\"Product\":\"MabiTools Channel Checker\",\"Version\":";
    string(out, metadata.version);
    out << ",\"BuildCommit\":";
    if (metadata.build_commit)
        string(out, *metadata.build_commit);
    else
        out << "null";
    const auto field = [&](const char* name, const char* value) {
        out << ',';
        string(out, name);
        out << ':';
        string(out, value);
    };
    field("Timing", report_text::Timing);
    field("Measurement", report_text::Measurement);
    field("Scope", report_text::Scope);
    field("Ranking", report_text::Ranking);
    field("PresetSource", report_text::PresetSource);
    field("PresetRevision", report_text::PresetRevision);
    field("EndpointNote", report_text::EndpointNote);
    out << ",\"Result\":{\"StartedAtUtc\":";
    string(out, utc_text(scan.started_utc));
    out << ",\"CompletedAtUtc\":";
    string(out, utc_text(scan.completed_utc));
    out << ",\"Options\":{\"Samples\":" << settings.samples
        << ",\"TimeoutMs\":" << settings.timeout_ms << ",\"IntervalMs\":" << settings.interval_ms
        << ",\"MaxConcurrency\":" << settings.max_concurrency << "},\"Channels\":[";
    const auto write_channel = [&](int index) {
        if (index < 0)
            out << "null";
        else
            channel_json(out, scan.values->channels.at(static_cast<std::size_t>(index)),
                         summary.channels.at(static_cast<std::size_t>(index)));
    };
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (i)
            out << ',';
        write_channel(static_cast<int>(i));
    }
    const auto& picks = summary.picks;
    out << "],\"Recommendations\":{\"LowestLatency\":";
    write_channel(picks.fastest);
    out << ",\"MostStable\":";
    write_channel(picks.consistent);
    out << ",\"BestOverall\":";
    write_channel(picks.overall);
    field("LowestLatencyStatus", status(picks.fastest_status));
    field("MostStableStatus", status(picks.consistent_status));
    field("BestOverallStatus", status(picks.overall_status));
    out << ",\"OverallResultsSimilar\":" << (picks.overall_results_similar ? "true" : "false")
        << "}}}";
    return out.str();
}
}
