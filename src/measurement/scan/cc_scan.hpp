#pragma once
#include "cc_core.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>

namespace cc::scanning {
// Blocking Windows scan API, independent of Qt. The host owns scheduling, worker
// lifetime, progress handoff, settings and history. This layer owns no UI loop.
struct endpoint {
    int number;
    std::string host;
    int port;
};

// Defaults match normal application startup.
// samples: 10..120 measured rounds, plus one excluded warm-up round.
// timeout_ms: 250..5000 milliseconds for connect and opening reply combined.
// interval_ms: 250..5000 milliseconds between round dispatches, including work and observer time.
// max_concurrency: 1..8 simultaneous probes.
struct options {
    int samples = 40, timeout_ms = 1500, interval_ms = 1000, max_concurrency = 4;
};

struct cancelled : std::exception {
    const char* what() const noexcept override { return "Scan cancelled"; }
};

// Software observations in milliseconds from one QueryPerformanceCounter (QPC)
// scan origin. Connection time survives a failed reply; total time exists only
// after all four bytes.
struct measurement {
    int round{};
    std::optional<double> total, connection;
    double start_offset{}, terminal_offset{}, elapsed{};
    std::string stage, outcome, failure;
    int socket_error{};
};

struct channel_measurements {
    endpoint target;
    std::vector<measurement> samples;
};

struct snapshot {
    std::vector<channel_measurements> channels;
    cc::scan_summary summary;
};

struct progress {
    int completed_attempts{}, total_attempts{};
    bool warming_up{};
    // An observer may retain this snapshot. Later rounds cannot mutate it.
    std::shared_ptr<const snapshot> values;
};

struct result {
    std::chrono::system_clock::time_point started_utc, completed_utc;
    std::shared_ptr<const snapshot> values;
};

using observer = std::function<void(const progress&)>;
// Called concurrently by scan's bounded workers. The callable and its captured
// transport must outlive scan, honour the stop token, and be thread-safe. Round 0
// is warm-up; measured rounds are 1..samples. scan_origin is a QPC counter value,
// not a UTC timestamp. Probe failures are measurements; cancellation throws.
// Release per-call resources before returning or throwing.
using probe_function = std::function<measurement(const endpoint&, int round, int timeout_ms,
                                                 std::int64_t scan_origin, std::stop_token)>;

// Monotonic QPC values are comparable only within the running machine/session.
// elapsed_ms converts their difference to milliseconds; use result UTC values
// for reporting wall time, never these counter values.
std::int64_t timestamp();
double elapsed_ms(std::int64_t from, std::int64_t to);
void check_cancel(std::stop_token cancellation);
// Validates 1..30 targets and the option limits above.
// Production permits only exact (number, host spelling, port) bundled entries,
// including subsets/reordering; tests permit 127.0.0.1 and ::1. Both reject
// duplicate channel numbers/host-port pairs and invalid limits. Throws
// invalid_argument without opening sockets. Parsing alone is not permission.
void validate(const std::vector<endpoint>& targets, options settings);

// Blocking coordinator: run on an owned worker, never the UI thread. Calls
// validate before dispatch and owns the copied targets for the whole scan.
// Every round joins all probe workers before publishing or propagating an error.
// Probe/observer exceptions propagate; cancellation throws cancelled without a
// partial result. Retained snapshots contain only complete measured rounds and
// remain valid after cancellation. No detached work is left behind.
// report runs synchronously on the calling worker, not a probe worker. Callers
// must enqueue snapshots themselves when handing progress to a UI thread.
result scan(std::vector<endpoint> targets, options settings, const probe_function& probe,
            const observer& report = {}, std::stop_token cancellation = {});

// Event-driven Winsock transport. The owner
// must outlive its synchronous calls; different worker threads may call it.
// Owns Winsock initialisation and per-call socket/event cleanup. probe validates
// target policy even when called directly, sends no payload, and succeeds after
// receiving four opening bytes. A failed connect has no connection time; a
// failed reply may retain one. Socket outcomes become measurements, while
// cancellation and infrastructure failures throw. No DNS resolution occurs.
class tcp_transport {
public:
    tcp_transport();
    ~tcp_transport();
    tcp_transport(const tcp_transport&) = delete;
    tcp_transport& operator=(const tcp_transport&) = delete;
    measurement probe(const endpoint&, int round, int timeout_ms, std::int64_t scan_origin,
                      std::stop_token cancellation);
    int active_probes() const;
    int peak_probes() const;

private:
    struct implementation;
    std::unique_ptr<implementation> state_;
};

const char* socket_name(int error);
}
