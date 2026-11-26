// One event-driven Winsock probe observes connection and four opening bytes
// against a shared monotonic deadline. A connection can succeed while the reply
// fails; retain that distinction in the measurement. Per-call RAII releases the
// socket, event and stop callback before control returns to the coordinator.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "cc_scan.hpp"
#include "deadline.hpp"
#include <array>
#include <atomic>
#include <cmath>

namespace cc::scanning {
namespace {
struct winsock_lifetime {
    winsock_lifetime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data))
            throw std::runtime_error("WSAStartup failed.");
    }

    ~winsock_lifetime() { WSACleanup(); }
};

struct socket_owner {
    SOCKET value;

    explicit socket_owner(int family) : value(socket(family, SOCK_STREAM, IPPROTO_TCP)) {
        if (value == INVALID_SOCKET)
            throw std::runtime_error("Socket creation failed.");
    }

    ~socket_owner() { closesocket(value); }

    socket_owner(const socket_owner&) = delete;
    socket_owner& operator=(const socket_owner&) = delete;
};

struct network_event {
    WSAEVENT value = WSACreateEvent();

    network_event() {
        if (value == WSA_INVALID_EVENT)
            throw std::runtime_error("Network event creation failed.");
    }

    ~network_event() { WSACloseEvent(value); }

    network_event(const network_event&) = delete;
    network_event& operator=(const network_event&) = delete;
};

std::int64_t frequency() {
    static const auto result = [] {
        LARGE_INTEGER value{};
        if (!QueryPerformanceFrequency(&value))
            throw std::runtime_error("QPC frequency unavailable.");
        return value.QuadPart;
    }();
    return result;
}
}

std::int64_t timestamp() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value))
        throw std::runtime_error("QPC failed.");
    return value.QuadPart;
}

double elapsed_ms(std::int64_t from, std::int64_t to) {
    // Stopwatch.GetElapsedTime represents its result in 100 ns TimeSpan ticks.
    const auto ticks = static_cast<std::int64_t>((to - from) * (10000000.0 / frequency()));
    return ticks / 10000.0;
}

struct tcp_transport::implementation {
    winsock_lifetime winsock;
    std::atomic<int> active{}, peak{};
};

tcp_transport::tcp_transport() : state_(std::make_unique<implementation>()) {
    (void)frequency();
}

tcp_transport::~tcp_transport() = default;

int tcp_transport::active_probes() const {
    return state_->active.load();
}

int tcp_transport::peak_probes() const {
    return state_->peak.load();
}

// Drive one non-blocking connection through connect, opening reply and terminal
// observation. Both phases share one deadline; the boundary object arbitrates
// cancellation/expiry before a timing can become a published measurement.
measurement tcp_transport::probe(const endpoint& target, int round, int timeout_ms,
                                 std::int64_t scan_origin, std::stop_token cancellation) {
    validate({target}, options{10, timeout_ms, 250, 1});
    check_cancel(cancellation);

    struct active_call {
        implementation& state;

        explicit active_call(implementation& value) : state(value) {
            const int active = ++state.active;
            int peak = state.peak.load();
            while (peak < active && !state.peak.compare_exchange_weak(peak, active)) {
            }
        }

        ~active_call() { --state.active; }
    } active(*state_);

    // Events and socket belong to this call. Registering network events makes
    // the socket non-blocking; the separate stop event wakes a blocked wait.
    const int family = target.host == "::1" ? AF_INET6 : AF_INET;
    network_event ready, cancelled_event;
    socket_owner client(family); // Close the socket before destroying its registered events.
    // stop_callback unregisters before either event is destroyed, including when
    // cancellation is requested from another thread during return/unwinding.
    std::stop_callback signal_cancel(cancellation, [&] { WSASetEvent(cancelled_event.value); });
    if (WSAEventSelect(client.value, ready.value, FD_CONNECT | FD_READ | FD_CLOSE))
        throw std::runtime_error("WSAEventSelect failed.");
    sockaddr_storage address{};
    int address_length{};
    if (family == AF_INET6) {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(address);
        v6.sin6_family = AF_INET6;
        v6.sin6_addr = in6addr_loopback;
        v6.sin6_port = htons(static_cast<u_short>(target.port));
        address_length = sizeof(v6);
    } else {
        auto& v4 = reinterpret_cast<sockaddr_in&>(address);
        v4.sin_family = AF_INET;
        v4.sin_port = htons(static_cast<u_short>(target.port));
#ifdef CC_BUNDLED_COMPARISON
        if (InetPtonA(AF_INET, target.host.c_str(), &v4.sin_addr) != 1)
            throw std::invalid_argument("Expected a bundled numeric IPv4 address.");
#else
        v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
        address_length = sizeof(v4);
    }
    // Start measurement after address/setup work. All waits use the remaining
    // budget from this origin, rather than granting the reply a second timeout.
    const auto started = timestamp();
    probe_boundary boundary(started, frequency(), timeout_ms);
    const auto observe = [&] {
        // Braced initialisers are evaluated left-to-right: timestamp first,
        // cancellation snapshot second. Function argument order would not ensure
        // that publication rule on every C++ compiler.
        return probe_observation{timestamp(), cancellation.stop_requested()};
    };
    const auto finish = [&](const terminal_observation& terminal) {
        // Arbitration already captured both the counter value and cancellation
        // snapshot. Formatting/cleanup never reclassifies a published result.
        if (terminal.outcome == probe_terminal::cancelled)
            throw cancelled{};
        const auto ended = terminal.counter;
        int error = terminal.socket_error;
        std::string outcome, failure;
        switch (terminal.outcome) {
        case probe_terminal::completed:
            outcome = "Completed";
            break;
        case probe_terminal::closed_before_response:
            outcome = "ClosedBeforeResponse";
            failure = "Connection closed before opening response";
            break;
        case probe_terminal::socket_failure:
            outcome = "SocketFailure";
            failure = "Connection failed: " + std::string(socket_name(error));
            break;
        case probe_terminal::read_failure:
            outcome = "ReadFailure";
            failure = "Opening response read failed";
            break;
        default:
            outcome = "TimedOut";
            failure = "Timed out";
            error = 0;
            break;
        }
        const auto elapsed = elapsed_ms(started, ended);
        const auto offset = elapsed_ms(scan_origin, started);
        const auto connected = boundary.connected_at();
        const auto connection_ms =
            connected ? std::optional(elapsed_ms(started, *connected)) : std::nullopt;
        const std::string stage = connected ? "OpeningResponse" : "Connect";
        return measurement{round,
                           outcome == "Completed" ? std::optional(elapsed) : std::nullopt,
                           connection_ms,
                           offset,
                           offset + elapsed,
                           elapsed,
                           stage,
                           std::move(outcome),
                           std::move(failure),
                           error};
    };
    const auto wait_network = [&]() -> std::optional<WSANETWORKEVENTS> {
        for (;;) {
            const auto observed = observe();
            if (boundary.poll(observed))
                return std::nullopt;
            const HANDLE waits[]{cancelled_event.value, ready.value};
            const auto status = WaitForMultipleObjects(
                2, waits, FALSE,
                static_cast<DWORD>(std::ceil(boundary.remaining_ms(observed.counter))));
            // Notifications wake the observer; only a fresh counter/cancellation
            // snapshot decides expiry. An early timer wake cannot invent timeout.
            if (status == WAIT_OBJECT_0 || status == WAIT_TIMEOUT)
                continue;
            if (status != WAIT_OBJECT_0 + 1)
                throw std::runtime_error("Socket wait failed.");
            WSANETWORKEVENTS events{};
            if (WSAEnumNetworkEvents(client.value, ready.value, &events))
                throw std::runtime_error("Event enumeration failed.");
            return events;
        }
    };
    if (const auto terminal = boundary.poll(observe()))
        return finish(*terminal);
    // Connect may complete immediately or signal FD_CONNECT later. Other network
    // events cannot advance the state; inspect the connect event's own error.
    if (connect(client.value, reinterpret_cast<const sockaddr*>(&address), address_length) ==
        SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK)
            return finish(boundary.failed(probe_terminal::socket_failure, error, observe()));
        for (;;) {
            const auto events = wait_network();
            if (!events)
                return finish(*boundary.terminal());
            if (!(events->lNetworkEvents & FD_CONNECT))
                continue;
            const int error_code = events->iErrorCode[FD_CONNECT_BIT];
            if (error_code)
                return finish(
                    boundary.failed(probe_terminal::socket_failure, error_code, observe()));
            break;
        }
    }
    // A connect readiness notification is not evidence of connection completion
    // before the deadline. Do not invent a connection time when first observed
    // at/after it; the timeout remains in the Connect stage.
    if (const auto terminal = boundary.connected(observe()))
        return finish(*terminal);
    // Accumulate exactly four opening bytes without sending a request. Short
    // reads remain in this phase; EOF, errors and expiry retain connection time.
    std::array<char, 4> seed{};
    for (;;) {
        if (const auto terminal = boundary.poll(observe()))
            return finish(*terminal);
        const int count = recv(client.value, seed.data() + boundary.received_bytes(),
                               static_cast<int>(seed.size()) - boundary.received_bytes(), 0);
        if (count >= 0) {
            // The pre-read check alone cannot decide this boundary: the fourth
            // byte may be observed after the deadline even though recv began
            // before it. A late full reply is a timeout, never a successful total.
            if (const auto terminal = boundary.read(count, observe()))
                return finish(*terminal);
            continue;
        }
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK)
            return finish(boundary.failed(probe_terminal::read_failure, error, observe()));
        if (!wait_network())
            return finish(*boundary.terminal());
        // Retry recv after readiness, draining queued data before EOF/reset.
        // A spurious ready signal must not count as a byte or complete reply.
    }
}

const char* socket_name(int error) {
    switch (error) {
    case WSAECONNREFUSED:
        return "ConnectionRefused";
    case WSAECONNRESET:
        return "ConnectionReset";
    case WSAECONNABORTED:
        return "ConnectionAborted";
    case WSAETIMEDOUT:
        return "TimedOut";
    case WSAEHOSTUNREACH:
        return "HostUnreachable";
    case WSAENETUNREACH:
        return "NetworkUnreachable";
    case WSAENETDOWN:
        return "NetworkDown";
    case WSAEACCES:
        return "AccessDenied";
    default:
        return "Unmapped"; // Preserve unknown numeric errors without inventing a name.
    }
}
}
