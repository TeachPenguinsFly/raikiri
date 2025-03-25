// Fuzz target for the bybit wire parsers.
//
// Venue JSON is untrusted input arriving at line rate on a process that
// holds live orders, so the failure this exists to prevent is concrete: a
// malformed or truncated frame taking down a gateway mid-position. Unit
// tests cover the shapes bybit documents; the fuzzer covers the ones it
// will eventually send anyway.
//
// The first input byte selects a parser so one binary and one corpus reach
// every entry point — libFuzzer mutates that byte like any other, so
// coverage spreads across parsers on its own. The remainder is fed verbatim,
// which keeps seed files usable as plain JSON: a captured frame prefixed by
// a selector byte is still a valid seed for the parser it selects.
//
// Nothing here asserts on the RESULT. Parsers are allowed to reject anything;
// what they may not do is read out of bounds, leak, or throw. ASan/UBSan and
// the noexcept-in-effect contract are the oracles.

#include <raikiri/venues/bybit/wire.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

using namespace raikiri::venues::bybit;

// Buffers are reused across runs exactly as a gateway reuses them per
// connection — a fresh buffer every iteration would hide the case where a
// parser leaves stale spans behind after failing.
OrderbookBuffer g_orderbook;
TradeBuffer g_trades;
OrderUpdateBuffer g_orders;
ExecutionBuffer g_executions;
PositionBuffer g_positions;
InstrumentBuffer g_instruments;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    const auto selector = data[0];
    const std::string_view frame{reinterpret_cast<const char*>(data) + 1, size - 1};

    switch (selector % 10) {
    case 0:
        (void)parse_topic(frame);
        break;
    case 1:
        (void)parse_orderbook(frame, g_orderbook);
        break;
    case 2:
        (void)parse_trades(frame, g_trades);
        break;
    case 3:
        (void)parse_ticker(frame);
        break;
    case 4:
        (void)parse_order_updates(frame, g_orders);
        break;
    case 5:
        (void)parse_executions(frame, g_executions);
        break;
    case 6:
        (void)parse_positions(frame, g_positions);
        break;
    case 7:
        (void)parse_envelope(frame);
        break;
    case 8:
        (void)parse_order_ack(frame);
        break;
    default:
        (void)parse_instruments(frame, g_instruments);
        break;
    }
    return 0;
}
