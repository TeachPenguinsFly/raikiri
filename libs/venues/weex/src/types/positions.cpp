#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer; a
// venue that reimplements opt_str or req_array is one that will disagree with
// its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

std::expected<void, WireError> read_position(ondemand::object& obj, WirePosition& out) {
    auto symbol = detail::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;
    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    out.side = field("side");
    out.size = field("size");
    obj.reset();
    out.updated_ms = detail::opt_u64(obj, "updatedTime").value_or(0);
    return {};
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded(
        [&] { return read_array<WirePosition>(body, storage.positions, read_position); });
}

} // namespace raikiri::venues::weex
