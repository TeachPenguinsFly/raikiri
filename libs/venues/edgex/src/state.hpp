#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/edgex/book.hpp>
#include <raikiri/venues/edgex/wire.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace raikiri::venues::edgex::internal {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    MarketType market;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    std::unordered_map<std::uint32_t, InstrumentId> by_contract;
    std::unordered_map<std::uint64_t, std::uint32_t> contract_of; // InstrumentId::packed

    std::unordered_map<std::uint32_t, Book> books;

    // Bbo and Depth share one depth key; Funding shares the one ticker.all.1s key
    std::unordered_set<std::string> channels;
    std::unordered_set<std::uint64_t> funding_wanted;
    // the book is also the quote, so a bbo-only gateway must not publish its ladder
    std::unordered_set<std::uint64_t> depth_wanted;

    std::unordered_set<std::uint32_t> priced;
    std::unordered_set<std::uint32_t> traded;
    std::unordered_set<std::uint32_t> funded;

    std::uint64_t subscribe_refusals{0};
    std::string last_refusal;
    std::uint64_t resubscribes{0};

    [[nodiscard]] const Instrument* instrument_of(std::uint32_t contract_id) const noexcept {
        const auto found = by_contract.find(contract_id);
        return found == by_contract.end() ? nullptr : instruments.find(found->second);
    }
};

} // namespace raikiri::venues::edgex::internal
