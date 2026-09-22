#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit {

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
    // An empty side with size "0" is bybit's way of saying flat.
    out.side = field("side");
    out.size = field("size");
    out.avg_price = field("entryPrice");
    out.updated_ms = wire::digits_to_u64(field("updatedTime"));
    return {};
}

} // namespace

std::expected<std::span<const WirePosition>, WireError>
parse_position_list(std::string_view result, PositionBuffer& storage) {
    return guarded([&] {
        return read_list<WirePosition>(
            result, "list", storage.positions, k_max_positions, read_position);
    });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view frame,
                                                                        PositionBuffer& storage) {
    return guarded([&] {
        return read_list<WirePosition>(
            frame,
            "data",
            storage.positions,
            k_max_positions,
            [](ondemand::object& obj, WirePosition& out) -> std::expected<void, WireError> {
                auto symbol = wire::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected{symbol.error()};
                }
                out.symbol = *symbol;
                const auto field = [&](std::string_view key) {
                    obj.reset();
                    return wire::opt_str(obj, key).value_or(std::string_view{});
                };
                // An empty side with size "0" is bybit's way of saying flat.
                out.side = field("side");
                out.size = field("size");
                out.avg_price = field("entryPrice");
                out.updated_ms = wire::digits_to_u64(field("updatedTime"));
                return {};
            });
    });
}

} // namespace raikiri::venues::bybit
