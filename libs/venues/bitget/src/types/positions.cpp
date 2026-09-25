#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitget {

namespace {

using wire::guarded;

std::expected<void, WireError> read_position(wire::ondemand::object& obj, WirePosition& out) {
    out.symbol = field(obj, "instId");
    if (out.symbol.empty()) {
        out.symbol = field(obj, "symbol");
    }
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.hold_side = field(obj, "holdSide");
    out.total = field(obj, "total");
    out.open_price_avg = field(obj, "openPriceAvg");
    obj.reset();
    out.update_ms = wire::opt_ms(obj, "uTime").value_or(0);
    return {};
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&] { return read_rows<WirePosition>(body, {}, storage.positions, read_position); });
}

} // namespace raikiri::venues::bitget
