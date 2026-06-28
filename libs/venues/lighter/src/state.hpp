#pragma once

// detail, not anonymous: an anonymous type in a header differs in every includer.

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/lighter/book.hpp>
#include <raikiri/venues/lighter/budget.hpp>
#include <raikiri/venues/lighter/ws.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::venues::lighter::detail {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* rest_budget{nullptr};
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    std::shared_ptr<MessageBudget> budget{process_budget()};

    std::unordered_map<std::uint32_t, InstrumentId> by_market;
    std::unordered_map<std::uint64_t, std::uint32_t> market_of; // InstrumentId::packed

    std::unordered_map<std::uint32_t, Book> books;

    std::unordered_set<std::string> channels;

    std::unordered_set<std::uint64_t> funding_wanted;
    // A BBO-only subscriber holds a book too, and must not have its ladder published.
    std::unordered_set<std::uint64_t> depth_wanted;

    std::unordered_set<std::uint32_t> traded;
    std::unordered_set<std::uint32_t> funded;

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
    std::uint64_t resubscribes{0};

    [[nodiscard]] const Instrument* instrument_of(std::uint32_t market_id) const noexcept {
        const auto found = by_market.find(market_id);
        return found == by_market.end() ? nullptr : instruments.find(found->second);
    }
};

} // namespace raikiri::venues::lighter::detail
