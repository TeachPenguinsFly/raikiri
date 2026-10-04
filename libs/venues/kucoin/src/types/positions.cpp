#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kucoin {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WirePosition>(
            *array, storage.positions, [](auto& obj, WirePosition& row) -> std::expected<void, WireError> {
                const auto symbol = req_decimal(obj, "symbol");
                const auto qty = opt_decimal(obj, "currentQty");
                const auto entry = opt_decimal(obj, "avgEntryPrice");
                const auto ts = opt_u64(obj, "currentTimestamp");
                if (!symbol.has_value() || !qty.has_value() || !entry.has_value() || !ts.has_value()) {
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

std::expected<WireClosedPositionPage, WireError> parse_history_positions(std::string_view body,
                                                                         ClosedPositionBuffer& storage) {
    return guarded([&]() -> std::expected<WireClosedPositionPage, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(*root, [&](wire::ondemand::object& obj) -> std::expected<WireClosedPositionPage, WireError> {
            const auto pages = opt_u64(obj, "totalPage");
            if (!pages.has_value()) {
                return std::unexpected(pages.error());
            }
            auto array = wire::req_array(obj, "items");
            if (!array.has_value()) {
                return std::unexpected(array.error());
            }
            const auto count = read_rows<WireClosedPosition>(
                *array,
                storage.positions,
                [](auto& row_obj, WireClosedPosition& row) -> std::expected<void, WireError> {
                    const auto symbol = req_decimal(row_obj, "symbol");
                    const auto closed = opt_u64(row_obj, "closeTime");
                    if (!symbol.has_value() || !closed.has_value()) {
                        return std::unexpected(WireError::WrongType);
                    }
                    row.symbol = *symbol;
                    row.close_ms = *closed;
                    return {};
                });
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            return WireClosedPositionPage{.positions = {storage.positions.data(), *count}, .total_pages = *pages};
        });
    });
}

} // namespace raikiri::venues::kucoin
