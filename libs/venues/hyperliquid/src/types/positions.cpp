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

std::expected<WireLeverage, WireError> parse_active_asset_leverage(std::string_view body) {
    return wire::guarded([&]() -> std::expected<WireLeverage, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto leverage = wire::req_object(*root, "leverage");
        if (!leverage.has_value()) {
            return std::unexpected(leverage.error());
        }
        WireLeverage out{};
        const auto type = wire::req_str(*leverage, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        out.type = *type;
        leverage->reset();
        const auto value = wire::opt_i64(*leverage, "value");
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        out.value = *value;
        return out;
    });
}

std::expected<std::string_view, WireError> parse_action_refusal(std::string_view body) {
    return wire::guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = wire::req_str(*root, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        if (*status == "ok") {
            return std::string_view{};
        }
        root->reset();
        const auto prose = wire::opt_str(*root, "response").value_or(std::string_view{});
        return prose.empty() ? std::string_view{"the venue refused without a reason"} : prose;
    });
}

} // namespace raikiri::venues::hyperliquid
