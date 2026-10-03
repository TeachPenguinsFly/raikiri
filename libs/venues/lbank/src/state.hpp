#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace raikiri::venues::lbank::internal {

// indexed by symbol id
struct Market {
    book::Ladders ladders;
    bool synced{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    bool checking{false};
    std::uint64_t last_bno{0};
    std::uint64_t last_trade_id{0};
    std::uint64_t touched_frame{0};
    std::uint32_t interval_s{0};
    InstrumentId book{};
    std::string tick_text;
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    MarketType market;
    bool has_credentials{false};
    exec::RateBudget* budget{nullptr};

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Market> markets;
    std::uint64_t max_bno{0};
    std::uint64_t frames{0};
    std::uint64_t next_app_ping_ns{0};
    std::uint64_t next_report_ns{0};
    std::uint64_t subscribe_refusals{0};
    std::uint64_t book_faults{0};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::lbank::internal
