#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/orders.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::binance {

namespace {

using wire::guarded;
using wire::session;

[[nodiscard]] std::expected<WireOrder, WireError> read_order(wire::ondemand::object& obj) {
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
    const auto executed = wire::opt_str(obj, "executedQty");
    if (!executed.has_value()) {
        return std::unexpected(executed.error());
    }
    out.executed_qty = *executed;
    const auto order_id = wire::opt_u64(obj, "orderId");
    const auto update_ms = wire::opt_u64(obj, "updateTime");
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

std::expected<WireOrderUpdate, WireError> parse_order_update(std::string_view data) {
    return guarded([&]() -> std::expected<WireOrderUpdate, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireOrderUpdate out{};
        const auto event_ms = wire::opt_u64(*root, "E");
        if (!event_ms.has_value()) {
            return std::unexpected(event_ms.error());
        }
        out.event_ms = *event_ms;

        auto order = wire::req_object(*root, "o");
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
        const auto commission = wire::opt_str(*order, "n");
        if (!commission.has_value()) {
            return std::unexpected(commission.error());
        }
        out.commission = *commission;

        const auto order_id = wire::opt_u64(*order, "i");
        const auto trade_id = wire::opt_u64(*order, "t");
        const auto trade_ms = wire::opt_u64(*order, "T");
        const auto maker = wire::opt_bool(*order, "m");
        const auto reduce_only = wire::opt_bool(*order, "R");
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
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
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
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
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
            const auto trade_id = wire::opt_u64(obj, "id");
            const auto order_id = wire::opt_u64(obj, "orderId");
            const auto time_ms = wire::opt_u64(obj, "time");
            const auto maker = wire::opt_bool(obj, "maker");
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

} // namespace raikiri::venues::binance
