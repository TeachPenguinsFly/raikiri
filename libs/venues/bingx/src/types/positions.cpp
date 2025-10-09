#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bingx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WirePosition out{};
            const auto symbol = detail::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            for (const auto& [key, field] : {std::pair{"positionSide", &out.position_side},
                                             std::pair{"positionAmt", &out.position_amt},
                                             std::pair{"avgPrice", &out.avg_price}}) {
                const auto value = opt_scalar(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto updated = opt_ms_any(obj, "updateTime");
            if (!updated.has_value()) {
                return std::unexpected(updated.error());
            }
            out.update_ms = *updated;
            storage.positions[count] = out;
            ++count;
        }
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

} // namespace raikiri::venues::bingx
