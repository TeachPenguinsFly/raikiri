#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/upbit/flavour.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::venues::upbit::internal {

// indexed by SymbolId
struct Market {
    bool booked{false};
    bool depth{false};
    bool priced{false};
    bool traded{false};
    bool volumed{false};
    std::uint64_t last_book_ts{0};
};

struct VenueState {
    VenueState(const Flavour& flavour_ref, exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : flavour{flavour_ref}, sink{&sink_ref}, clock{&clock_ref} {}

    Flavour flavour;
    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Market> markets;
    std::uint64_t subscribe_refusals{0};
    std::uint64_t stale_books{0};
    std::uint64_t refused_books{0};

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::upbit::internal
