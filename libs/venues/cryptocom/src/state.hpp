#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/cryptocom/book.hpp>
#include <raikiri/venues/cryptocom/market_data.hpp>
#include <raikiri/venues/cryptocom/ws.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::venues::cryptocom::internal {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // one book channel feeds both the quote and the ladder
    std::unordered_map<std::uint64_t, Book> books;
    std::unordered_set<std::uint64_t> wants_bbo;
    std::unordered_set<std::uint64_t> wants_depth;

    std::unordered_map<std::uint64_t, FundingMemo> funding;
    // a perpetual's index name from the catalogue, and the perpetuals each subscribed index feeds
    std::unordered_map<std::uint64_t, std::string> index_of;
    std::unordered_map<std::string, std::vector<InstrumentId>> index_perps;

    std::unordered_set<std::string> desired;

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
    std::uint64_t book_resubscribes{0};
    std::uint64_t trade_replays_dropped{0};
};

inline std::optional<net::RestResponse>
await_response(VenueState& state, net::RestClient& rest, const net::RestRequest& request, std::uint64_t timeout_ms) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::cryptocom::internal
