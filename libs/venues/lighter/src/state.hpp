#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/lighter/book.hpp>
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
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // Every frame names its market by integer id and nothing else, so this is
    // the lookup the whole data path runs on. Filled with the catalogue.
    std::unordered_map<std::uint32_t, InstrumentId> by_market;
    std::unordered_map<std::uint64_t, std::uint32_t> market_of; // InstrumentId::packed

    std::unordered_map<std::uint32_t, Book> books;

    // The channels asked for, as a set: Bbo and Depth are ONE order_book key,
    // and Funding on every perp is one market_stats/all key.
    std::unordered_set<std::string> channels;

    // Instruments the gateway asked funding for. The stream is one `all`
    // subscription, so this is what decides which of its rows reach the sink.
    std::unordered_set<std::uint64_t> funding_wanted;
    // The book is also the quote, so a gateway that asked for BBO alone still
    // holds one — and must not have its ladder published.
    std::unordered_set<std::uint64_t> depth_wanted;

    // DISTINCT markets, for the stop line: a frame count says a venue is busy,
    // not that the catalogue is covered.
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
