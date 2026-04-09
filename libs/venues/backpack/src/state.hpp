#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/backpack/book.hpp>
#include <raikiri/venues/backpack/endpoints.hpp>
#include <raikiri/venues/backpack/rest.hpp>
#include <raikiri/venues/backpack/ws.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::venues::backpack::detail {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    exec::RateBudget* budget{nullptr};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // Book seeds and the 24-hour table. Four warm connections: a whole
    // catalogue is 143 seeds, and two connections against a round trip is
    // what left a neighbour venue's books cold for a minute.
    net::RestClient rest{
        io, net::RestConfig{.host = std::string{ep::rest_host(testnet)}, .pool_size = 4}};
    std::unordered_map<std::uint64_t, DepthBook> books;
    std::vector<InstrumentId> pending_seeds;
    std::uint64_t next_seed_ns{0};

    // BOOKTICKER SENDS NO SNAPSHOT: it speaks when the touch moves and not
    // before. Measured 2026-09-24 across all 143 Open markets: the first frame
    // took 5 s at the median, 14 s at p90 and 42 s at worst, and two spot
    // pairs said nothing for 90 s while their books sat two-sided. So every
    // quote subscription is seeded once from a five-level REST snapshot, and
    // `quoted` keeps a slow answer from overwriting a live frame that beat it.
    std::vector<InstrumentId> pending_touches;
    std::unordered_set<std::uint64_t> quoted;

    // Instruments the gateway asked funding for. On a perpetual that is the
    // markPrice stream; on either market it is also the turnover the tickers
    // poll publishes, which is all the funding channel means for a spot pair.
    std::vector<InstrumentId> funding_instruments;

    std::uint64_t subscribe_refusals{0};
};

// Makes one REST client ask the budget before every send. RateClass::Poll:
// nothing here is an order, and a refusal leaves the caller due, so the next
// tick asks again.
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    auto* owner = &state;
    rest.set_admit([owner](std::string_view) {
        if (owner->budget == nullptr) {
            return true; // unmeasured: unlimited, not closed
        }
        return owner->budget
            ->take(exec::RateBucket::Shared,
                   ep::k_request_cost,
                   owner->clock->mono(),
                   exec::RateClass::Poll)
            .has_value();
    });
}

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::backpack::detail
