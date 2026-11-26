#pragma once

// Schema-7 report explanations, including the native transport's shared
// connection-and-read deadline and compatibility fields used by saved reports.
namespace cc::scanning::report_text {
inline constexpr const char* Timing =
    R"CC(Timing offsets are monotonic milliseconds from the scan origin near StartedAtUtc, observed in software, not packet arrival times. ElapsedMs includes failed attempts; failed LatencyMs remains null. Timing is null for historical samples without these observations. TimeoutMs defines one monotonic deadline for connect and read together. Caller cancellation observed at publication returns no sample; otherwise a terminal observation at or after the deadline is TimedOut, including a complete reply. Failure elapsed time may exceed the budget. This is a software-observation deadline, not a wire-arrival deadline.)CC";
inline constexpr const char* Measurement =
    R"CC(Connection time measures time taken to establish a TCP connection. Reply wait is the additional time after connecting until the channel's four-byte opening message has been read. Total time combines both. These are sequential stages, not outbound/inbound timings or in-game latency. AverageMs, min/max, P95, JitterMs and Fastest connection use Connection time from complete successful checks. Most consistent uses Jitter, measured from Total time (ResponseJitterMs). Graphs show Connection in teal and Reply as a dashed blue line in Connection / Reply view, or combined time in the default Total time view. The suggested channel uses successful checks, Total time and Jitter. One warm-up probe per channel is excluded.)CC";
inline constexpr const char* Scope =
    R"CC(This checks the connection. Battle performance can differ. Results describe this connection, not party-wide channel health.)CC";
inline constexpr const char* Ranking =
    R"CC(At least two qualified channels with matching rounds are needed. Each qualifies independently in the full scan and each half: 5 successes, 3 adjacent successful pairs and 80% success. Fastest connection picks the lowest average Connection time; Most consistent picks the lowest Jitter. The suggested channel (BestOverall) favours the highest successful-check rate, then lowest average Total time + 2 × Jitter. Exact ties use channel number. BestMeasured is a practical pick without an established clear lead. ClearLead requires an advantage over every qualified rival in the full scan and both halves: at least 2 ms and 5% for connection/overall, 1 ms and 10% for consistency. Overall also requires no lower success rate; a 10 percentage-point success advantage qualifies instead of a score advantage. OverallResultsSimilar indicates an alternative within the full-scan overall margin and under a 10-point success-rate gap. These are practical comparison rules, not statistical confidence or validated combat-performance thresholds.)CC";
inline constexpr const char* PresetSource = R"CC(MabiTools Erinn NA)CC";
inline constexpr const char* PresetRevision = R"CC(2026.11.23)CC";
inline constexpr const char* EndpointNote =
    R"CC(Each channel result records the actual address used, including any custom edits.)CC";
}
