#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::mexc {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;

std::expected<void, WireError> read_futures_position(detail::ondemand::object& obj,
                                                     WireFuturesPosition& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.hold_vol = field(obj, "holdVol");
    out.hold_avg_price = field(obj, "holdAvgPrice");
    out.position_type = int_field(obj, "positionType");
    out.state = int_field(obj, "state");
    out.update_ms = detail::digits_to_u64(field(obj, "updateTime"));
    return {};
}

} // namespace

std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage) {
    return guarded([&] {
        return read_envelope_array<WireFuturesPosition>(
            body, storage.positions, read_futures_position);
    });
}

std::expected<WireFuturesPosition, WireError> parse_futures_position(std::string_view frame) {
    return guarded(
        [&] { return read_envelope_object<WireFuturesPosition>(frame, read_futures_position); });
}

} // namespace raikiri::venues::mexc
