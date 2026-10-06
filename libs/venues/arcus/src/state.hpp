#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/arcus/book.hpp>
#include <raikiri/venues/arcus/map.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace raikiri::venues::arcus::internal {

// indexed by SymbolId
struct Market {
    bool funding{false};
    bool depth{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    std::uint64_t last_quote_seq{0};
    std::uint64_t last_trade_id{0};
    std::optional<Book> book;
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
    bool stats_asked{false};
    std::uint64_t next_report_ns{0};
    std::uint64_t subscribe_refusals{0};
    std::uint64_t resubscribes{0};
    std::uint64_t degraded{0};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::arcus::internal
