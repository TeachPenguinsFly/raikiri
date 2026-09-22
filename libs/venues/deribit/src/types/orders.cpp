#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/orders.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::deribit {

namespace {

using wire::guarded;
using wire::session;

// A number field the venue documents as sometimes carrying a WORD instead:
// `price` is "market_price" on an open trigger market order. The word reads as
// empty so the mapper drops the price rather than the whole order.
std::expected<std::string_view, WireError> lenient_num(wire::ondemand::object& obj,
                                                       std::string_view key) {
    const auto token = opt_num(obj, key);
    if (token.has_value()) {
        return *token;
    }
    return token.error() == WireError::WrongType ? std::expected<std::string_view, WireError>{}
                                                 : token;
}

std::expected<WireOrder, WireError> read_order(wire::ondemand::object& obj) {
    WireOrder out{};
    for (const auto& [key, field] : {std::pair{"order_id", &out.order_id},
                                     std::pair{"label", &out.label},
                                     std::pair{"instrument_name", &out.instrument},
                                     std::pair{"order_state", &out.state},
                                     std::pair{"time_in_force", &out.tif}}) {
        const auto value = wire::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    if (out.order_id.empty() || out.instrument.empty()) {
        return std::unexpected(WireError::MissingField);
    }

    const auto price = lenient_num(obj, "price");
    if (!price.has_value()) {
        return std::unexpected(price.error());
    }
    out.price = *price;
    for (const auto& [key, field] : {std::pair{"amount", &out.amount},
                                     std::pair{"filled_amount", &out.filled_amount},
                                     std::pair{"average_price", &out.average_price}}) {
        const auto value = opt_num(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }

    const auto direction = wire::opt_str(obj, "direction");
    if (!direction.has_value()) {
        return std::unexpected(direction.error());
    }
    if (direction->empty()) {
        return std::unexpected(WireError::MissingField); // a side we must not guess
    }
    out.sell = *direction == "sell";

    const auto updated = opt_ms(obj, "last_update_timestamp");
    if (!updated.has_value()) {
        return std::unexpected(updated.error());
    }
    out.ts_ms = *updated;
    if (out.ts_ms == 0) {
        const auto created = opt_ms(obj, "creation_timestamp");
        if (!created.has_value()) {
            return std::unexpected(created.error());
        }
        out.ts_ms = *created;
    }
    return out;
}

std::expected<WireUserTrade, WireError> read_user_trade(wire::ondemand::object& obj) {
    WireUserTrade out{};
    for (const auto& [key, field] : {std::pair{"trade_id", &out.trade_id},
                                     std::pair{"order_id", &out.order_id},
                                     std::pair{"label", &out.label},
                                     std::pair{"instrument_name", &out.instrument},
                                     std::pair{"fee_currency", &out.fee_currency},
                                     std::pair{"state", &out.state}}) {
        const auto value = wire::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    if (out.trade_id.empty() || out.instrument.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    for (const auto& [key, field] : {std::pair{"price", &out.price},
                                     std::pair{"amount", &out.amount},
                                     std::pair{"fee", &out.fee}}) {
        const auto value = opt_num(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }

    const auto direction = wire::opt_str(obj, "direction");
    if (!direction.has_value()) {
        return std::unexpected(direction.error());
    }
    if (direction->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.sell = *direction == "sell";

    const auto liquidity = wire::opt_str(obj, "liquidity");
    if (!liquidity.has_value()) {
        return std::unexpected(liquidity.error());
    }
    // Absent on some historical rows, and Unknown is a legitimate answer the
    // domain already models — guessing Taker would flatter a maker strategy's
    // own headline metric.
    out.liquidity_stated = !liquidity->empty();
    out.maker = *liquidity == "M";

    const auto ts = opt_ms(obj, "timestamp");
    if (!ts.has_value()) {
        return std::unexpected(ts.error());
    }
    out.ts_ms = *ts;
    return out;
}

} // namespace

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body,
                                                       UserTradeBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        auto order_obj = wire::req_object(*result, "order");
        if (!order_obj.has_value()) {
            return std::unexpected(order_obj.error());
        }
        const auto order = read_order(*order_obj);
        if (!order.has_value()) {
            return std::unexpected(order.error());
        }
        const auto count = read_rows<WireUserTrade>(
            *result, "trades", storage.trades, [](wire::ondemand::object& obj) {
                return read_user_trade(obj);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return WireOrderAck{.order = *order,
                            .trades =
                                std::span<const WireUserTrade>{storage.trades.data(), *count}};
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                  OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto count = read_rows<WireOrder>(
            *root, "result", storage.orders, [](wire::ondemand::object& obj) {
                return read_order(obj);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireOrder>{storage.orders.data(), *count};
    });
}

std::expected<WireChanges, WireError>
parse_user_changes(std::string_view frame, OrderBuffer& orders, UserTradeBuffer& trades) {
    return guarded([&]() -> std::expected<WireChanges, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        // A frame that moved only positions carries neither array, and that is
        // not an error — it is simply nothing this adapter acts on.
        std::size_t order_count = 0;
        if (wire::has_field(*data, "orders")) {
            const auto count = read_rows<WireOrder>(
                *data, "orders", orders.orders, [](wire::ondemand::object& obj) {
                    return read_order(obj);
                });
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            order_count = *count;
        }
        std::size_t trade_count = 0;
        if (wire::has_field(*data, "trades")) {
            const auto count = read_rows<WireUserTrade>(
                *data, "trades", trades.trades, [](wire::ondemand::object& obj) {
                    return read_user_trade(obj);
                });
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            trade_count = *count;
        }
        return WireChanges{.orders = std::span<const WireOrder>{orders.orders.data(), order_count},
                           .trades =
                               std::span<const WireUserTrade>{trades.trades.data(), trade_count}};
    });
}

std::expected<std::span<const WireUserTrade>, WireError>
parse_user_trades(std::string_view body, UserTradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireUserTrade>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto count = read_rows<WireUserTrade>(
            *result, "trades", storage.trades, [](wire::ondemand::object& obj) {
                return read_user_trade(obj);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireUserTrade>{storage.trades.data(), *count};
    });
}

} // namespace raikiri::venues::deribit
