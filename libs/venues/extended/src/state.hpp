#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/extended/book.hpp>
#include <raikiri/venues/extended/map.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::extended::internal {

// What the gateway asked for on one instrument, and what the streams have said
// about it. Indexed by SymbolId.
struct Market {
    // The book of an RFQ market is maker quotes, not resting orders: a price,
    // never executable depth.
    bool rfq{false};
    bool quote{false};
    bool depth{false};
    bool trades{false};
    bool funding{false};
    // Set while the depth=1 socket has spoken for this market since it opened;
    // until then the full book's level zero is the quote.
    bool fast_quote{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    std::uint64_t last_trade_id{0};
    FundingMemo memo{};
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag, exec::RateBudget* budget_ptr)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag}, budget{budget_ptr} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    exec::RateBudget* budget;

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Market> markets;
    std::unordered_map<std::uint32_t, Book> books;

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::extended::internal
