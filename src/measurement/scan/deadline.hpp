#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace cc::scanning {
// Candidate outcomes describe what the socket operation reported. A timeout is
// decided from the same monotonic deadline for connection and all reply bytes,
// never from the order in which Windows delivered readiness/timer notifications.
enum class probe_terminal {
    completed,
    closed_before_response,
    socket_failure,
    read_failure,
    timed_out,
    cancelled
};

class probe_deadline {
public:
    // Counter units are QPC ticks; the budget is milliseconds. Rounding upward
    // chooses the first representable tick at or after the requested deadline.
    // This arithmetic is independent of the 100 ns rounding used in reports.
    probe_deadline(std::int64_t started, std::int64_t ticks_per_second, int budget_ms)
        : frequency_(ticks_per_second) {
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        if (started < 0 || ticks_per_second <= 0 || budget_ms <= 0 ||
            ticks_per_second > (maximum - 999) / budget_ms)
            throw std::invalid_argument("Invalid probe deadline clock or budget.");
        const auto duration = (ticks_per_second * budget_ms + 999) / 1000;
        if (started > maximum - duration)
            throw std::invalid_argument("Probe deadline overflow.");
        due_ = started + duration;
    }

    bool reached(std::int64_t observed) const noexcept { return observed >= due_; }

    double remaining_ms(std::int64_t observed) const noexcept {
        return reached(observed)
                   ? 0.
                   : static_cast<double>(due_ - observed) * 1000. / static_cast<double>(frequency_);
    }

    // The caller records the terminal observation time, then samples cancellation
    // once for publication. That snapshot is the decision boundary: cancellation
    // wins; otherwise a deadline tie/overrun becomes TimedOut, including EOF or a
    // socket error. Before the deadline, preserve the observed socket outcome.
    // There is no later stop-token read that could retract this decision. The
    // scan coordinator can still discard an entire round on caller cancellation.
    std::optional<probe_terminal> decide(std::optional<probe_terminal> candidate,
                                         std::int64_t observed,
                                         bool cancellation_observed) const noexcept {
        if (cancellation_observed)
            return probe_terminal::cancelled;
        if (reached(observed))
            return probe_terminal::timed_out;
        return candidate;
    }

private:
    std::int64_t frequency_;
    std::int64_t due_{};
};

struct terminal_observation {
    probe_terminal outcome;
    std::int64_t counter;
    int socket_error;
    bool operator==(const terminal_observation&) const = default;
};

struct probe_observation {
    std::int64_t counter;
    bool cancellation_observed;
};

// This state belongs to one probe thread. Windows operations provide observed
// counter values and a caller-cancellation snapshot; tests provide controlled
// values to the same methods. Readiness notifications do not count as bytes.
// Once published, a terminal observation is immutable, including its timestamp.
class probe_boundary {
public:
    probe_boundary(std::int64_t started, std::int64_t frequency, int budget_ms)
        : deadline_(started, frequency, budget_ms) {}

    double remaining_ms(std::int64_t observed) const noexcept {
        return deadline_.remaining_ms(observed);
    }

    int received_bytes() const noexcept { return received_; }

    std::optional<std::int64_t> connected_at() const noexcept { return connected_; }

    const std::optional<terminal_observation>& terminal() const noexcept { return terminal_; }

    std::optional<terminal_observation> poll(probe_observation observation) {
        return publish(std::nullopt, observation);
    }

    std::optional<terminal_observation> connected(probe_observation observation) {
        if (const auto result = poll(observation))
            return result;
        if (connected_)
            throw std::logic_error("Connection was observed twice.");
        connected_ = observation.counter;
        return {};
    }

    std::optional<terminal_observation> read(int count, probe_observation observation) {
        if (terminal_)
            return terminal_;
        if (!connected_ || count < 0 || count > 4 - received_)
            throw std::logic_error("Invalid opening-response observation.");
        received_ += count;
        const auto candidate = count == 0 ? std::optional(probe_terminal::closed_before_response)
                               : received_ == 4 ? std::optional(probe_terminal::completed)
                                                : std::nullopt;
        return publish(candidate, observation);
    }

    terminal_observation failed(probe_terminal failure, int socket_error,
                                probe_observation observation) {
        if (failure != probe_terminal::socket_failure && failure != probe_terminal::read_failure)
            throw std::logic_error("Expected a socket failure observation.");
        return *publish(failure, observation, socket_error);
    }

private:
    std::optional<terminal_observation> publish(std::optional<probe_terminal> candidate,
                                                probe_observation observation,
                                                int socket_error = 0) {
        if (terminal_)
            return terminal_;
        if (const auto outcome = deadline_.decide(candidate, observation.counter,
                                                  observation.cancellation_observed)) {
            const bool socket_failure = *outcome == probe_terminal::socket_failure ||
                                        *outcome == probe_terminal::read_failure;
            terminal_ = terminal_observation{*outcome, observation.counter,
                                             socket_failure ? socket_error : 0};
        }
        return terminal_;
    }

    probe_deadline deadline_;
    std::optional<std::int64_t> connected_;
    int received_{};
    std::optional<terminal_observation> terminal_;
};
}
