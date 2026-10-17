#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

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
        storage.margin = {};
        wire::ondemand::value summary;
        if (const auto ec = wire::find(*root, "marginSummary").get(summary); ec == simdjson::SUCCESS) {
            wire::ondemand::object fields;
            if (const auto bad = summary.get_object().get(fields); bad != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(bad));
            }
            const auto value = wire::opt_str(fields, "accountValue");
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            storage.margin.account_value = *value;
        } else if (ec != simdjson::NO_SUCH_FIELD) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto withdrawable = wire::opt_str(*root, "withdrawable");
        if (!withdrawable.has_value()) {
            return std::unexpected(withdrawable.error());
        }
        storage.margin.withdrawable = *withdrawable;
        auto rows = wire::req_array(*root, "assetPositions");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto row : *rows) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object wrapper;
            if (const auto ec = row.get_object().get(wrapper); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            auto position = wire::req_object(wrapper, "position");
            if (!position.has_value()) {
                return std::unexpected(position.error());
            }
            const auto coin = wire::req_str(*position, "coin");
            const auto size = wire::req_str(*position, "szi");
            const auto entry = wire::req_str(*position, "entryPx");
            const auto pnl = wire::opt_str(*position, "unrealizedPnl");
            if (!coin.has_value() || !size.has_value() || !entry.has_value() || !pnl.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.positions[count] = {.coin = *coin, .size = *size, .entry_px = *entry, .unrealized_pnl = *pnl};
            ++count;
        }
        // `time` trails assetPositions, and on-demand cannot go back
        const auto ts = opt_number_ms(*root, "time");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        storage.ts_ms = *ts;
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

} // namespace raikiri::venues::hyperliquid
