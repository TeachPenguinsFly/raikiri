#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/dydx/book.hpp>
#include <raikiri/venues/dydx/wire.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace raikiri::venues::dydx::detail {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    std::unordered_map<std::uint64_t, Book> books; // InstrumentId::packed

    std::unordered_set<std::uint64_t> depth_wanted;
    std::unordered_set<std::uint64_t> trades_wanted;
    std::unordered_set<std::uint64_t> funding_wanted;

    // What the venue said `subscribed` to, by ticker. A key the pool holds and
    // this does not is one the venue refused or has not answered yet.
    std::unordered_set<std::string> acked_books;
    std::unordered_set<std::string> acked_trades;

    // Loaded tickers the catalogue poll has since seen leave ACTIVE.
    std::unordered_set<std::string> halted;

    std::unordered_set<std::uint64_t> traded;
    std::unordered_set<std::uint64_t> funded;

    std::uint64_t subscribe_refusals{0};
    std::string last_refusal;
    std::uint64_t resubscribes{0};
    std::uint64_t overflows{0};
};

} // namespace raikiri::venues::dydx::detail
