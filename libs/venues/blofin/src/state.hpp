#pragma once

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/blofin/trade.hpp>

#include <cstdint>
#include <string>

namespace raikiri::venues::blofin::internal {

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

    net::Signer signer;
    std::string passphrase;
    // per order id, what the orders channel has reported filled so far
    ProgressTable progress;
    std::int64_t last_error_code{0};
    std::uint64_t private_frames_dropped{0};
};

} // namespace raikiri::venues::blofin::internal
