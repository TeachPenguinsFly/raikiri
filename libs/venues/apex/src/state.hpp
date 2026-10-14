#pragma once

// internal namespace: an anonymous one in a header is a distinct type per file

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/apex/book.hpp>
#include <raikiri/venues/apex/map.hpp>
#include <raikiri/venues/apex/trade.hpp>
#include <raikiri/venues/apex/ws.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace raikiri::venues::apex::internal {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    // unloaded on a market-data-only gateway; the catalogue load fills its signing metadata
    RequestBuilder signing;

    // the order symbol (BTC-USDT) both ways; streams name the cross symbol instead
    std::unordered_map<std::string, InstrumentId> by_symbol;
    std::unordered_map<std::uint64_t, std::string> symbol_of; // InstrumentId::packed

    std::unordered_map<std::uint64_t, BookSync> books; // InstrumentId::packed
    std::unordered_map<std::uint64_t, FundingMemo> funding;
    std::unordered_set<std::string> topics;
    std::unordered_set<std::uint64_t> depth_wanted;

    std::unordered_set<std::uint64_t> quoted;
    std::unordered_set<std::uint64_t> traded;
    std::unordered_set<std::uint64_t> funded;
    std::uint64_t subscribe_refusals{0};
    std::uint64_t resubscribes{0};

    [[nodiscard]] const Instrument* instrument_of(std::string_view symbol) const {
        const auto found = by_symbol.find(std::string{symbol});
        return found == by_symbol.end() ? nullptr : instruments.find(found->second);
    }
};

} // namespace raikiri::venues::apex::internal
