#pragma once

// internal namespace: an anonymous one in a header is a distinct type per translation unit

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/rest.hpp>
#include <raikiri/venues/weex/ws.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::venues::weex::internal {

// shared per gateway process; single threaded, nothing locks
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    // the contract tree lists only perps; kept because instrument ids carry it
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // market-data builder; the signed half holds its own since builders share scratch
    RequestBuilder builder;

    // weex signs with key, secret and passphrase; without the passphrase it answers -1049
    net::Signer signer;
    std::string passphrase;

    // offset to /capi/v3/market/time; signatures are refused 30 s off
    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    // catalogue and funding poll share one client and the per-ip allowance
    net::RestClient rest{io, net::RestConfig{.host = std::string{rest_host()}}};

    std::unordered_map<std::uint64_t, DepthBook> books;
    std::unordered_map<std::uint64_t, BookWatch> book_watch;
    std::uint64_t next_reseed_scan_ns{0};

    // a set: Bbo and Depth name one channel
    std::vector<std::string> desired;

    // instruments the funding poll fans out to; empty means nobody asked
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // contracts an order may name, from /capi/v3/market/apiTradingSymbols; empty means unknown, so nothing is refused
    std::unordered_set<std::string> api_tradable;

    [[nodiscard]] bool orderable(std::string_view symbol) const noexcept {
        return api_tradable.empty() || api_tradable.contains(std::string{symbol});
    }

    // subscriptions the venue refused; the reason is prose
    std::uint64_t subscribe_refusals{0};
    std::string last_error;

    // adds a channel if it is new; returns true when it was
    [[nodiscard]] bool add_channel(std::string_view channel) {
        for (const auto& existing : desired) {
            if (existing == channel) {
                return false; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(channel);
        return true;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// market-data and signed reads share one per-ip weight bucket; poll class at a flat cost of 1
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    rest.set_admit(exec::market_data_gate(state.budget, *state.clock));
}

inline constexpr std::size_t k_max_unanswered = 512;

// offset to weex's clock for the 30 s signature window
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::weex::internal
