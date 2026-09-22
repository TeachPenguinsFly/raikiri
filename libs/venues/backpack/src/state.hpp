#pragma once

// internal, not anonymous: an anonymous type in a header differs in every includer.

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

namespace raikiri::venues::backpack::internal {

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

    net::RestClient rest{
        io, net::RestConfig{.host = std::string{ep::rest_host(testnet)}, .pool_size = 4}};
    std::unordered_map<std::uint64_t, DepthBook> books;
    std::vector<InstrumentId> pending_seeds;
    std::uint64_t next_seed_ns{0};

    // bookTicker sends no snapshot, so each quote is seeded once from REST; `quoted`
    // keeps a slow answer from overwriting a live frame that beat it.
    std::vector<InstrumentId> pending_touches;
    std::unordered_set<std::uint64_t> quoted;

    // On a spot pair, funding means only the turnover the tickers poll publishes.
    std::vector<InstrumentId> funding_instruments;

    std::uint64_t subscribe_refusals{0};
};

// A refusal leaves the caller due, so the next tick asks again.
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    rest.set_admit(exec::market_data_gate(state.budget, *state.clock));
}

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::backpack::internal
