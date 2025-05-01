#include "wire_detail.hpp"

namespace raikiri::venues::binance {

namespace {

using detail::guarded;
using detail::session;

// Reads a run of string fields off one object in a single pass.
struct Wanted {
    std::string_view key;
    std::string_view* into;
};

[[nodiscard]] std::expected<void, WireError> read_strings(detail::ondemand::object& obj,
                                                          std::span<const Wanted> wanted) {
    for (const auto& want : wanted) {
        const auto value = detail::opt_str(obj, want.key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *want.into = *value;
    }
    return {};
}

} // namespace

std::expected<WireOrderUpdate, WireError> parse_order_update(std::string_view data) {
    return guarded([&]() -> std::expected<WireOrderUpdate, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireOrderUpdate out{};
        const auto event_ms = detail::opt_u64(*root, "E");
        if (!event_ms.has_value()) {
            return std::unexpected(event_ms.error());
        }
        out.event_ms = *event_ms;

        auto order = detail::req_object(*root, "o");
        if (!order.has_value()) {
            return std::unexpected(order.error());
        }
        const std::array<Wanted, 13> wanted{{
            {"s", &out.symbol},
            {"c", &out.client_order_id},
            {"x", &out.exec_type},
            {"X", &out.order_status},
            {"S", &out.side},
            {"o", &out.order_type},
            {"f", &out.time_in_force},
            {"q", &out.order_qty},
            {"p", &out.order_price},
            {"l", &out.last_filled_qty},
            {"z", &out.cum_filled_qty},
            {"L", &out.last_filled_price},
            {"N", &out.commission_asset},
        }};
        if (const auto ok = read_strings(*order, wanted); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        const auto commission = detail::opt_str(*order, "n");
        if (!commission.has_value()) {
            return std::unexpected(commission.error());
        }
        out.commission = *commission;

        const auto order_id = detail::opt_u64(*order, "i");
        const auto trade_id = detail::opt_u64(*order, "t");
        const auto trade_ms = detail::opt_u64(*order, "T");
        const auto maker = detail::opt_bool(*order, "m");
        const auto reduce_only = detail::opt_bool(*order, "R");
        if (!order_id.has_value() || !trade_id.has_value() || !trade_ms.has_value() ||
            !maker.has_value() || !reduce_only.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.order_id = *order_id;
        out.trade_id = *trade_id;
        out.trade_ms = *trade_ms;
        out.is_maker = *maker;
        out.reduce_only = *reduce_only;

        if (out.symbol.empty() || out.exec_type.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WirePosition>, WireError>
parse_account_update(std::string_view data, PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto account = detail::req_object(*root, "a");
        if (!account.has_value()) {
            return std::unexpected(account.error());
        }
        auto positions = detail::req_array(*account, "P");
        if (!positions.has_value()) {
            return std::unexpected(positions.error());
        }
        std::size_t count = 0;
        for (auto entry : *positions) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WirePosition out{};
            const std::array<Wanted, 4> wanted{{{"s", &out.symbol},
                                                {"pa", &out.position_amt},
                                                {"ep", &out.entry_price},
                                                {"ps", &out.position_side}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            storage.positions[count] = out;
            ++count;
        }
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

namespace {

[[nodiscard]] std::expected<WireOrder, WireError> read_order(detail::ondemand::object& obj) {
    WireOrder out{};
    const std::array<Wanted, 6> wanted{{{"symbol", &out.symbol},
                                        {"clientOrderId", &out.client_order_id},
                                        {"status", &out.status},
                                        {"side", &out.side},
                                        {"price", &out.price},
                                        {"origQty", &out.orig_qty}}};
    if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    const auto executed = detail::opt_str(obj, "executedQty");
    if (!executed.has_value()) {
        return std::unexpected(executed.error());
    }
    out.executed_qty = *executed;
    const auto order_id = detail::opt_u64(obj, "orderId");
    const auto update_ms = detail::opt_u64(obj, "updateTime");
    if (!order_id.has_value() || !update_ms.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.order_id = *order_id;
    out.update_ms = *update_ms;
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

} // namespace

std::expected<WireOrder, WireError> parse_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_order(*root);
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                  OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto& sess = session();
        auto array = sess.open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_orders) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto order = read_order(obj);
            if (!order.has_value()) {
                return std::unexpected(order.error());
            }
            storage.orders[count] = *order;
            ++count;
        }
        return std::span<const WireOrder>{storage.orders.data(), count};
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_user_trades(std::string_view body,
                                                                       TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireTrade out{};
            const std::array<Wanted, 5> wanted{{{"symbol", &out.symbol},
                                                {"price", &out.price},
                                                {"qty", &out.qty},
                                                {"commission", &out.commission},
                                                {"side", &out.side}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            const auto trade_id = detail::opt_u64(obj, "id");
            const auto order_id = detail::opt_u64(obj, "orderId");
            const auto time_ms = detail::opt_u64(obj, "time");
            const auto maker = detail::opt_bool(obj, "maker");
            if (!trade_id.has_value() || !order_id.has_value() || !time_ms.has_value() ||
                !maker.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.trade_id = *trade_id;
            out.order_id = *order_id;
            out.time_ms = *time_ms;
            out.maker = *maker;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<std::span<const WirePositionRisk>, WireError>
parse_position_risk(std::string_view body, std::span<WirePositionRisk> storage) {
    return guarded([&]() -> std::expected<std::span<const WirePositionRisk>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == storage.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WirePositionRisk out{};
            const std::array<Wanted, 3> wanted{{{"symbol", &out.symbol},
                                                {"positionAmt", &out.position_amt},
                                                {"entryPrice", &out.entry_price}}};
            if (const auto ok = read_strings(obj, wanted); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            storage[count] = out;
            ++count;
        }
        return std::span<const WirePositionRisk>{storage.data(), count};
    });
}

std::expected<std::string_view, WireError> parse_listen_key(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto key = detail::opt_str(*root, "listenKey");
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
        if (key->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return *key;
    });
}

} // namespace raikiri::venues::binance
