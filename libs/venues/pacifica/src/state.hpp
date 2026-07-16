#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/pacifica/map.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::venues::pacifica::detail {

// Indexed by SymbolId.
struct Market {
    bool rfq{false};
    bool funding{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    LastId quote{};
    LastId book{};
    FundingRoll roll{};
    std::uint64_t last_history_id{0};
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref,
               const Clock& clock_ref,
               bool testnet_flag,
               exec::RateBudget* budget_ptr)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag}, budget{budget_ptr} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    exec::RateBudget* budget;

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Market> markets;
    bool prices_asked{false};
    std::uint64_t next_app_ping_ns{0};
    std::uint64_t next_report_ns{0};
    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::pacifica::detail
