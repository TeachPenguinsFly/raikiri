#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/positions.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::xt {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<void, WireError> read_futures_position(detail::ondemand::object& obj,
                                                     WireFuturesPosition& out) {
    for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                     std::pair{"positionSide", &out.position_side},
                                     std::pair{"positionSize", &out.position_size},
                                     std::pair{"entryPrice", &out.entry_price},
                                     std::pair{"contractType", &out.contract_type}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

} // namespace

std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFuturesPosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WireFuturesPosition>(
            *array, storage.positions, [](auto& obj, WireFuturesPosition& row) {
                return read_futures_position(obj, row);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireFuturesPosition>{storage.positions.data(), *count};
    });
}

} // namespace raikiri::venues::xt
