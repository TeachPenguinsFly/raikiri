#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace raikiri::venues::derive::internal {

// indexed by SymbolId
struct Market {
    bool priced{false};
    bool greeked{false};
    bool traded{false};
    bool funded{false};
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    MarketType market;
    exec::RateBudget* budget{nullptr};
    // comma-separated underlyings to load; empty loads all
    std::string underlyings;

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Market> markets;
    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::derive::internal
