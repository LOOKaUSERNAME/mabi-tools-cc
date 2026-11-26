// Run warm-up and measured rounds with bounded probe concurrency. Each round
// joins before its immutable snapshot is published; partial rounds are discarded
// on cancellation or error. The caller owns the outer worker and observer.

#include "cc_scan.hpp"
#ifdef CC_BUNDLED_COMPARISON
#include "comparison_catalog.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

namespace cc::scanning {
void check_cancel(std::stop_token cancellation) {
    if (cancellation.stop_requested())
        throw cancelled{};
}

void validate(const std::vector<endpoint>& targets, options settings) {
    if (targets.empty() || targets.size() > 30)
        throw std::invalid_argument("Choose between 1 and 30 channels.");
    if (settings.samples < 10 || settings.samples > 120 || settings.timeout_ms < 250 ||
        settings.timeout_ms > 5000 || settings.interval_ms < 250 || settings.interval_ms > 5000 ||
        settings.max_concurrency < 1 || settings.max_concurrency > 8)
        throw std::invalid_argument(
            "Use 10-120 samples, 250-5000 ms timeout/interval and 1-8 parallel probes.");
    std::set<int> numbers;
    std::set<std::pair<std::string, int>> addresses;
    for (const auto& target : targets) {
        if (target.number < 1 || target.number > 99 || target.port < 1 || target.port > 65535)
            throw std::invalid_argument("Invalid channel number or port.");
#ifdef CC_BUNDLED_COMPARISON
        if (std::none_of(comparison_catalog.begin(), comparison_catalog.end(),
                         [&](const auto& allowed) {
                             return target.number == allowed.number &&
                                    target.host == allowed.host && target.port == allowed.port;
                         }))
            throw std::invalid_argument(
                "Comparison only accepts exact bundled Erinn channel entries.");
#else
        if (target.host != "127.0.0.1" && target.host != "::1")
            throw std::invalid_argument(
                "Only canonical IPv4/IPv6 loopback is enabled in this test transport.");
#endif
        if (!numbers.insert(target.number).second ||
            !addresses.emplace(target.host, target.port).second)
            throw std::invalid_argument("Channels and address/port pairs must be distinct.");
    }
}

namespace {
std::shared_ptr<const snapshot> capture(const std::vector<channel_measurements>& channels) {
    std::vector<cc::channel> inputs;
    for (const auto& channel : channels) {
        cc::channel input{channel.target.number, {}};
        for (const auto& sample : channel.samples)
            input.samples.push_back(
                {sample.round, sample.total, sample.connection, !sample.failure.empty()});
        inputs.push_back(std::move(input));
    }
    return std::make_shared<const snapshot>(snapshot{channels, cc::summarize(inputs)});
}

void interval_wait(double milliseconds, std::stop_token cancellation) {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    changed.wait_for(lock, cancellation, std::chrono::duration<double, std::milli>(milliseconds),
                     [] { return false; });
    check_cancel(cancellation);
}
}

// Validate once, then run a discarded warm-up followed by complete measured
// rounds. Workers fill distinct result slots; only this calling thread appends
// samples and publishes snapshots after the round has joined.
result scan(std::vector<endpoint> targets, options settings, const probe_function& probe,
            const observer& report, std::stop_token cancellation) {
    validate(targets, settings);
    if (!probe)
        throw std::invalid_argument("A probe implementation is required.");
    std::vector<channel_measurements> channels;
    for (const auto& target : targets)
        channels.push_back({target, {}});

    const auto started_utc = std::chrono::system_clock::now();
    const auto origin = timestamp();
    const int count = static_cast<int>(targets.size());
    const auto publish = [&](int attempts, bool warming = false) {
        if (report)
            report({attempts, count * settings.samples, warming, capture(channels)});
    };
    check_cancel(cancellation);
    publish(0, true);

    for (int round = 0; round <= settings.samples; ++round) {
        check_cancel(cancellation);
        if (round == 1)
            publish(0);
        const auto round_started = timestamp();
        std::vector<measurement> finished(targets.size());
        std::atomic<int> next_offset{};
        std::mutex error_mutex;
        std::exception_ptr first_error;
        std::atomic<bool> worker_cancelled{};
        {
            // Fixed worker count bounds sockets and waiting work. No worker
            // publishes progress or touches the accumulated per-channel lists.
            std::vector<std::jthread> workers;
            for (int worker = 0; worker < std::min(count, settings.max_concurrency); ++worker) {
                workers.emplace_back([&] {
                    for (;;) {
                        const int offset = next_offset.fetch_add(1);
                        if (offset >= count)
                            return;
                        // Claim each channel once. Warm-up and round 1 start at
                        // index 0; later rounds rotate by one so the same channel
                        // does not always lead dispatch. Store by original index,
                        // not completion order, to keep catalogue order stable.
                        const auto index =
                            static_cast<std::size_t>((offset + std::max(0, round - 1)) % count);
                        try {
                            check_cancel(cancellation);
                            finished[index] = probe(targets[index], round, settings.timeout_ms,
                                                    origin, cancellation);
                        } catch (const cancelled&) {
                            worker_cancelled = true;
                        } catch (...) {
                            std::lock_guard lock(error_mutex);
                            if (!first_error)
                                first_error = std::current_exception();
                        }
                    }
                });
            }
        } // All workers and their sockets finish before inspecting a round.

        // Inspect only after joining: an error takes precedence over cancellation,
        // so scheduling order cannot hide a failed probe behind a cancelled one.
        if (first_error)
            std::rethrow_exception(first_error);
        if (worker_cancelled)
            throw cancelled{};
        check_cancel(cancellation);

        // Discard warm-up timings. Publish a measured round only when every
        // channel returned and no cancellation was observed after the join.
        if (round > 0) {
            for (std::size_t index = 0; index < channels.size(); ++index)
                channels[index].samples.push_back(std::move(finished[index]));
            publish(round * count);
        }
        if (round < settings.samples) {
            // As in ProbeEngine, interval is measured from round dispatch,
            // including a synchronous observer. Slow observers delay only the
            // next round; an overrun does not add another whole interval.
            const double remaining_ms =
                settings.interval_ms - elapsed_ms(round_started, timestamp());
            if (remaining_ms > 0)
                interval_wait(remaining_ms, cancellation);
        }
    }
    check_cancel(cancellation);
    return {started_utc, std::chrono::system_clock::now(), capture(channels)};
}
}
