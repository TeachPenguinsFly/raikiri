#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/apex/book.hpp>
#include <raikiri/venues/apex/map.hpp>
#include <raikiri/venues/apex/ws.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace raikiri::venues::apex::detail {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    std::unordered_map<std::uint64_t, BookSync> books; // InstrumentId::packed
    std::unordered_map<std::uint64_t, FundingMemo> funding;
    std::unordered_set<std::string> topics;
    std::unordered_set<std::uint64_t> depth_wanted;

    std::unordered_set<std::uint64_t> quoted;
    std::unordered_set<std::uint64_t> traded;
    std::unordered_set<std::uint64_t> funded;
    std::uint64_t subscribe_refusals{0};
    std::uint64_t resubscribes{0};
};

} // namespace raikiri::venues::apex::detail
