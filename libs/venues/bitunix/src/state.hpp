#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/bitunix/endpoints.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace raikiri::venues::bitunix::internal {

// indexed by SymbolId
struct Market {
    bool funding{false};
    bool priced{false};
    bool traded{false};
    bool funded{false};
    // when the trade subscription last went out; the first push after it is a backfill
    std::uint64_t trade_subscribed_ms{0};
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    MarketType market;
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    net::Signer signer;
    std::vector<Market> markets;
    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] std::string_view rest_host() const noexcept {
        return market == MarketType::Spot ? ep::k_spot_host : ep::k_perp_host;
    }

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::bitunix::internal
