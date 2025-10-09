#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kucoin {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
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
        const auto count = read_rows<WirePosition>(
            *array,
            storage.positions,
            [](auto& obj, WirePosition& row) -> std::expected<void, WireError> {
                const auto symbol = req_decimal(obj, "symbol");
                const auto qty = opt_decimal(obj, "currentQty");
                const auto entry = opt_decimal(obj, "avgEntryPrice");
                const auto ts = opt_u64(obj, "currentTimestamp");
                if (!symbol.has_value() || !qty.has_value() || !entry.has_value() ||
                    !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                row.symbol = *symbol;
                row.current_qty = *qty;
                row.avg_entry_price = *entry;
                row.ts_ms = *ts;
                return {};
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WirePosition>{storage.positions.data(), *count};
    });
}

} // namespace raikiri::venues::kucoin
