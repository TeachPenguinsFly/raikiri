#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/rest.hpp>
#include <raikiri/venues/weex/ws.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::weex::detail {

// State every WEEX connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};

    // ONE MARKET, and it is Perp. The contract tree lists nothing else and the
    // spot exchange is other hosts entirely, so this is a constant rather than
    // a choice — kept as a field because the id of every instrument carries it.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // The catalogue and the funding poll share one client: both are cold reads
    // against the same host and the same per-IP allowance, and a second pool
    // would spend that allowance twice while each half counted only its own.
    net::RestClient rest{io, net::RestConfig{.host = std::string{rest_host()}}};

    std::unordered_map<std::uint64_t, DepthBook> books;

    // Channels the gateway asked for, owned as strings because the instrument
    // table's views outlive nothing in particular. A SET: Bbo and Depth name
    // one channel here, so subscribing both places one key.
    std::vector<std::string> desired;

    // Funding is not a channel. What the poll fans out to is this list, and an
    // empty one means nobody asked, which is why the poll never fires for a
    // gateway that only wants prints.
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // Subscriptions the venue refused, and the last thing it said about one.
    // WEEX states no code on the socket, so the reason is prose.
    std::uint64_t subscribe_refusals{0};
    std::string last_error;

    // Adds a channel if it is new; returns true when it was.
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

// Makes the market-data REST client ask the budget before every send.
//
// EVERYTHING THIS ADAPTER SENDS IS MARKET DATA, so without this the venue's
// allowance would be spent entirely by traffic no counter in the process could
// see. WEEX meters WEIGHT per IP — its own exchangeInfo states 500 per ten
// minutes — and every public call this adapter makes is weight 1, so a flat
// cost of 1 is the venue's own arithmetic rather than an approximation.
//
// RateClass::Poll, never Order: nothing here may draw the bucket into a reserve
// an order path would need. A refusal is not an error — the funding poll asks
// again on its next tick.
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    auto* owner = &state;
    rest.set_admit([owner](std::string_view) {
        if (owner->budget == nullptr) {
            return true; // unmeasured venue: unlimited, not closed
        }
        return owner->budget
            ->take(exec::RateBucket::Shared, 1, owner->clock->mono(), exec::RateClass::Poll)
            .has_value();
    });
}

} // namespace raikiri::venues::weex::detail
