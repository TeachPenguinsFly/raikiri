#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/bullish/map.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::venues::bullish::internal {

// indexed by SymbolId
struct Market {
    bool ticked{false};
    bool booked{false};
    bool traded_wanted{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    bool volume{false};
    SeqRange book{};
    std::uint64_t last_trade_id{0};
    PriceTicks index{};
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
    std::uint64_t next_report_ns{0};
    std::uint64_t next_keepalive_ns{0};
    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::bullish::internal
