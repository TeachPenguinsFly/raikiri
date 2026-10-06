#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::venues::bitstamp::internal {

// indexed by SymbolId
struct Market {
    bool depth{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    bool volumed{false};
    std::uint64_t last_book_us{0};
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    MarketType market;
    exec::RateBudget* budget{nullptr};

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Market> markets;
    std::uint64_t subscribe_refusals{0};
    std::uint64_t reconnect_requests{0};
    std::uint64_t stale_books{0};
    std::uint64_t refused_books{0};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::bitstamp::internal
