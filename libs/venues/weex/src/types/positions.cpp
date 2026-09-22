#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
namespace ondemand = wire::ondemand;

std::expected<void, WireError> read_position(ondemand::object& obj, WirePosition& out) {
    auto symbol = wire::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;
    const auto field = [&](std::string_view key) {
        obj.reset();
        return wire::opt_str(obj, key).value_or(std::string_view{});
    };
    out.side = field("side");
    out.size = field("size");
    obj.reset();
    out.updated_ms = wire::opt_u64(obj, "updatedTime").value_or(0);
    return {};
}

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded(
        [&] { return read_array<WirePosition>(body, storage.positions, read_position); });
}

} // namespace raikiri::venues::weex
