#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/polymarketperps/map.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::polymarketperps::internal {

// indexed by SymbolId
struct Market {
    bool bbo{false};
    bool depth{false};
    bool trades{false};
    bool funding{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    LastSeq quote{};
    LastSeq book{};
    std::uint64_t next_funding_ns{0};
    Stats stats{};
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, exec::RateBudget* budget_ptr)
        : sink{&sink_ref}, clock{&clock_ref}, budget{budget_ptr} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget;

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Market> markets;
    // the stream names an instrument by its numeric id
    std::unordered_map<std::uint64_t, std::uint32_t> by_iid;
    std::uint64_t subscribe_refusals{0};
    bool seed_due{false};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::polymarketperps::internal
