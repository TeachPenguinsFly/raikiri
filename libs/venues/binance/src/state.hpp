#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::binance::detail {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // Shared with the account poller: binance counts one pool of weight per IP,
    // so two private buckets would together spend twice the allowance.
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder{signer};

    // The depth seed rides its own pool: a book resynchronising must never
    // queue behind an order, and an order must never queue behind a 40 kB
    // snapshot body.
    //
    // SIX WARM CONNECTIONS, AND THE NUMBER IS THE POINT. RestConfig defaults to
    // two, which is right for order entry — one order in flight and a spare —
    // and wrong for a catalogue that wants 571 snapshots at once: two
    // connections against a 290 ms round trip is under seven requests a second,
    // so the ROUND TRIP decided how long the venue stayed blind and the budget
    // below never got a word in. Measured on a full-catalogue start before this
    // was widened: 46 283 of 46 917 seed attempts refused by the connection
    // pool before the admit gate was ever asked, `x-mbx-used-weight-1m` peaking
    // at 554 of 2400, and ninety seconds during which most of the catalogue
    // published no ladder at all.
    net::RestClient depth_rest{
        io, net::RestConfig{.host = std::string{rest_host(testnet)}, .pool_size = 6}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<std::string> desired_streams;

    // Instruments the gateway asked funding for. The rate is PULLED, not
    // streamed: @markPrice is acknowledged on this venue and then never sends
    // a frame, so a gateway that trusted the stream carried no funding at all.
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // BOOK SEEDS ARE THE EXPENSIVE ONES, and they are REST, not the socket:
    // the pool paces SUBSCRIBE frames, and nothing it does paces these. Every
    // subscribed instrument wants one the moment its first delta arrives, and
    // at the limit=1000 this used to ask for that was weight 20 apiece — 571
    // listings is 11420 against an allowance of 2400 a minute, which is how
    // this process earned an IP ban twice on 2026-09-06 from traffic no
    // counter in it could see. The seed is 100 levels now (weight 5), the
    // queue below paces it, and meter_market_data charges it.
    std::vector<InstrumentId> pending_seeds;
    std::uint64_t next_seed_ns{0};

    // Venue clock offset, measured against /fapi/v1/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::string listen_key;
    std::uint64_t listen_key_due_ns{0};
    bool listen_key_expired{false};

    // PRIVATE FRAMES THIS PROCESS THREW AWAY. Each one is an order update the
    // book will never hear about, and all three ways of losing one used to be
    // a bare `return` — no counter, no line. A gateway can run for hours with a
    // connected private socket, book nothing, and look healthy.
    std::uint64_t private_frames_dropped{0};

    std::function<void(UnixNanos)> tick_hook;

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_stream(std::string_view stream) {
        for (const auto& existing : desired_streams) {
            if (existing == stream) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_streams.emplace_back(stream);
    }
};

// Market-data REST spends the venue's limits table as a poll; costs come from config.
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    rest.set_admit(exec::market_data_gate(state.budget, *state.clock));
}

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::binance::detail
