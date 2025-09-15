#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::kucoin::detail {

// The bullet response states 18 s; this is the fallback if it ever omits it.
constexpr std::uint64_t k_default_ping_ms = 18'000;

// Long enough that a venue outage does not turn into a bullet-public flood,
// short enough that a token rejection costs one second of data.
constexpr std::uint64_t k_reopen_delay_ns = 1'000'000'000ULL;

struct OwnedSubscription {
    std::string topic;
    std::string symbol;
};

// What a private socket needs to open. KuCoin puts a one-shot token in the
// stream URL and states the ping cadence beside it, and only a signed REST
// call mints one — which is why the socket cannot open before the REST pool
// has, and why this vocabulary is shared by both halves rather than owned by
// either.
struct PrivateTicket {
    StreamEndpoint endpoint;
    std::uint64_t ping_interval_ms{0};
};

// gatewayd pumps DataClient::poll and nothing else, so anything with a timer
// of its own registers here. The private sockets need one: KuCoin closes a
// connection whose application-level ping stops arriving, and the websocket
// control frame does not count as one.
class Pumped {
public:
    Pumped() = default;
    Pumped(const Pumped&) = delete;
    Pumped& operator=(const Pumped&) = delete;

    virtual void pump(UnixNanos now) noexcept = 0;

protected:
    ~Pumped() = default;
};

// State every kucoin connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, so nothing locks.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    // The one market this process serves. KuCoin's two trees are separate
    // deployments reached through separate bullets, and they share a subject
    // name ("level2") for two different payloads, so nothing below can read a
    // frame without knowing which socket it arrived on.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;
    Credentials credentials;

    std::vector<OwnedSubscription> desired;
    std::vector<Pumped*> pumps;

    // The seed rides its own pool so a book resynchronising never queues
    // behind the bullet token or the catalogue.
    net::RestClient depth_rest{io,
                               net::RestConfig{.host = std::string{rest_host()}, .port = "443"}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::uint64_t app_ping_interval_ns{k_default_ping_ms * 1'000'000ULL};
    std::uint64_t next_app_ping_ns{0};

    bool reopen_due{false};
    std::uint64_t reopen_at_ns{0};
    std::uint64_t connect_seq{0};

    // Measured against /api/v1/timestamp. KuCoin refuses signatures more than
    // five seconds from ITS clock, so a laptop drifting looks exactly like a
    // bad key until the offset is applied.
    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    void add_subscription(std::string_view topic, std::string_view symbol) {
        for (const auto& existing : desired) {
            if (existing.topic == topic && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{topic}, std::string{symbol});
    }

    void schedule_reopen() noexcept {
        reopen_due = true;
        reopen_at_ns = clock->mono().ns + k_reopen_delay_ns;
    }

    [[nodiscard]] std::string next_connect_id() {
        return "raikiri" + std::to_string(clock->wall().ns + connect_seq++);
    }

    DepthBook& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    std::optional<net::RestResponse> result;
    if (!rest.send(request,
                   [&result](net::RestResponse&& response) { result = std::move(response); })) {
        return std::nullopt;
    }
    const auto deadline = state.clock->mono().ns + timeout_ms * 1'000'000ULL;
    while (!result.has_value() && state.clock->mono().ns < deadline) {
        if (state.io.poll() == 0) {
            state.io.run_for(5);
        }
    }
    return result;
}

// Turns a REST answer into a usable body, or into the classified failure the
// gateway acts on.
inline std::expected<std::string, exec::VenueError>
usable_body(const std::optional<net::RestResponse>& response) {
    if (!response.has_value()) {
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Connectivity, 0, "no response before deadline")};
    }
    if (response->transport_failed()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
    }
    const auto envelope = parse_rest_envelope(response->body);
    if (!envelope.has_value()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
    }
    if (envelope->code != k_ok) {
        return std::unexpected{
            exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
    }
    return response->body;
}

} // namespace raikiri::venues::kucoin::detail
