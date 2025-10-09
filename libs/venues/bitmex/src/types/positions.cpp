#include <raikiri/venues/bitmex/envelope.hpp>
#include <raikiri/venues/bitmex/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitmex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

[[nodiscard]] std::expected<WirePosition, WireError> read_position(detail::ondemand::object& obj) {
    WirePosition out{};
    const auto symbol = detail::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected(symbol.error());
    }
    out.symbol = *symbol;
    const auto strings = read_strings(obj, {{"timestamp", &out.timestamp}});
    if (!strings.has_value()) {
        return std::unexpected(strings.error());
    }
    const auto numbers = read_numbers(
        obj, {{"currentQty", &out.current_qty}, {"avgEntryPrice", &out.avg_entry_price}});
    if (!numbers.has_value()) {
        return std::unexpected(numbers.error());
    }
    return out;
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view payload,
                                                                        PositionBuffer& storage) {
    return read_rows<WirePosition>(payload, storage.positions, read_position);
}

} // namespace raikiri::venues::bitmex
