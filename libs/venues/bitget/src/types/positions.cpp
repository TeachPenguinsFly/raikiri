#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitget {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;

std::expected<void, WireError> read_position(detail::ondemand::object& obj, WirePosition& out) {
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
    out.update_ms = detail::opt_ms(obj, "uTime").value_or(0);
    return {};
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded(
        [&] { return read_rows<WirePosition>(body, {}, storage.positions, read_position); });
}

} // namespace raikiri::venues::bitget
