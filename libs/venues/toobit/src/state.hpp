#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/toobit/map.hpp>
#include <raikiri/venues/toobit/rest.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::toobit::internal {

// indexed by SymbolId.
struct Market {
    bool priced{false};
    bool traded{false};
    bool funded{false};
    bool booked{false};
    bool wants_funding{false};
    bool wants_ticker{false};
    std::uint64_t last_trade_id{0};
    PriceTicks mark{};
    std::optional<FundingUpdate> funding;
    DepthBook book;
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, exec::RateBudget* budget_ptr)
        : sink{&sink_ref}, clock{&clock_ref}, budget{budget_ptr} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    RequestBuilder builder{signer};
    exec::InstrumentTable instruments;
    std::vector<Market> markets;

    std::uint64_t next_ping_ns{0};
    std::uint64_t next_funding_ns{0};
    std::uint64_t next_ticker_ns{0};
    std::uint64_t next_report_ns{0};
    std::uint64_t subscribe_refusals{0};
    std::uint64_t book_breaks{0};

    std::string listen_key;
    std::uint64_t listen_key_due_ns{0};
    bool listen_key_expiring{false};
    std::uint64_t private_frames_dropped{0};
    std::vector<std::string> ordered_symbols;

    // the data client's poll drives the listen key; the exec client installs this.
    std::function<void(UnixNanos)> tick_hook;

    [[nodiscard]] Market* market_of(InstrumentId id) noexcept {
        const auto index = id.symbol().raw;
        return index < markets.size() ? &markets[index] : nullptr;
    }
};

} // namespace raikiri::venues::toobit::internal
