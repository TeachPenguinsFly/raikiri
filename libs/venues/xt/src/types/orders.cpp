#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::xt {

namespace {

using wire::guarded;
using wire::session;

std::expected<void, WireError>
take_status(wire::ondemand::object& obj, std::string_view key, WireOrderStatus& into) {
    const auto text = opt_scalar(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    into = order_status(*text);
    return {};
}

// takerMaker case differs: TAKER on trade-list, taker on /v4/trade
bool text_is_maker(std::string_view text) noexcept {
    return text.size() == 5 && (text[0] == 'M' || text[0] == 'm');
}

// payload under result on rest reads and data on the socket
std::expected<wire::ondemand::object, WireError> payload_of(wire::ondemand::object& root) {
    if (wire::has_field(root, "result")) {
        return wire::req_object(root, "result");
    }
    return wire::req_object(root, "data");
}

std::expected<void, WireError> read_futures_order(wire::ondemand::object& obj,
                                                  WireFuturesOrder& out) {
    for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                     std::pair{"orderId", &out.order_id},
                                     std::pair{"clientOrderId", &out.client_order_id},
                                     std::pair{"price", &out.price},
                                     std::pair{"origQty", &out.orig_qty},
                                     std::pair{"executedQty", &out.executed_qty},
                                     std::pair{"avgPrice", &out.avg_price},
                                     std::pair{"orderSide", &out.order_side},
                                     std::pair{"positionSide", &out.position_side},
                                     std::pair{"orderType", &out.order_type},
                                     std::pair{"timeInForce", &out.time_in_force}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    if (const auto ok = take_status(obj, "state", out.state); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    const auto created = opt_ms(obj, "createdTime");
    const auto updated = opt_ms(obj, "updatedTime");
    if (!created.has_value() || !updated.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.created_ms = *created;
    out.updated_ms = *updated;
    if (out.symbol.empty() || out.order_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<void, WireError> read_futures_deal(wire::ondemand::object& obj,
                                                 WireFuturesDeal& out) {
    for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                     std::pair{"execId", &out.exec_id},
                                     std::pair{"orderId", &out.order_id},
                                     std::pair{"clientOrderId", &out.client_order_id},
                                     std::pair{"price", &out.price},
                                     std::pair{"quantity", &out.quantity},
                                     std::pair{"fee", &out.fee},
                                     std::pair{"orderSide", &out.order_side}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    // rest sends takerMaker text, the socket an isMaker boolean
    const auto taker_maker = opt_scalar(obj, "takerMaker");
    if (!taker_maker.has_value()) {
        return std::unexpected(taker_maker.error());
    }
    if (!taker_maker->empty()) {
        out.maker = text_is_maker(*taker_maker);
        out.liquidity_known = true;
    } else if (wire::has_field(obj, "isMaker")) {
        const auto is_maker = opt_bool_or(obj, "isMaker", false);
        if (!is_maker.has_value()) {
            return std::unexpected(is_maker.error());
        }
        out.maker = *is_maker;
        out.liquidity_known = true;
    }
    // some tenants repeat timestamp as time; fallback
    const auto ts = opt_ms(obj, "timestamp");
    if (!ts.has_value()) {
        return std::unexpected(ts.error());
    }
    out.ts_ms = *ts;
    if (out.ts_ms == 0) {
        const auto alt = opt_ms(obj, "time");
        if (!alt.has_value()) {
            return std::unexpected(alt.error());
        }
        out.ts_ms = *alt;
    }
    if (out.symbol.empty() || out.order_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<void, WireError> read_spot_order(wire::ondemand::object& obj, WireSpotOrder& out) {
    for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                     std::pair{"orderId", &out.order_id},
                                     std::pair{"clientOrderId", &out.client_order_id},
                                     std::pair{"side", &out.side},
                                     std::pair{"type", &out.type},
                                     std::pair{"timeInForce", &out.time_in_force},
                                     std::pair{"price", &out.price},
                                     std::pair{"origQty", &out.orig_qty},
                                     std::pair{"executedQty", &out.executed_qty},
                                     std::pair{"leavingQty", &out.leaving_qty},
                                     std::pair{"avgPrice", &out.avg_price},
                                     std::pair{"fee", &out.fee}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    if (const auto ok = take_status(obj, "state", out.state); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    // updatedTime stamps the lifecycle event; time is creation
    const auto updated = opt_ms(obj, "updatedTime");
    const auto created = opt_ms(obj, "time");
    if (!updated.has_value() || !created.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.ts_ms = *updated != 0 ? *updated : *created;
    if (out.symbol.empty() || out.order_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<void, WireError> read_spot_trade(wire::ondemand::object& obj, WireSpotTrade& out) {
    for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                     std::pair{"tradeId", &out.trade_id},
                                     std::pair{"orderId", &out.order_id},
                                     std::pair{"orderSide", &out.side},
                                     std::pair{"price", &out.price},
                                     std::pair{"quantity", &out.quantity},
                                     std::pair{"fee", &out.fee}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    const auto taker_maker = opt_scalar(obj, "takerMaker");
    if (!taker_maker.has_value()) {
        return std::unexpected(taker_maker.error());
    }
    if (!taker_maker->empty()) {
        out.maker = text_is_maker(*taker_maker);
        out.liquidity_known = true;
    }
    const auto ts = opt_ms(obj, "time");
    if (!ts.has_value()) {
        return std::unexpected(ts.error());
    }
    out.ts_ms = *ts;
    if (out.symbol.empty() || out.order_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

} // namespace

WireOrderStatus order_status(std::string_view text) noexcept {
    if (text == "NEW") {
        return WireOrderStatus::New;
    }
    if (text == "PARTIALLY_FILLED") {
        return WireOrderStatus::PartiallyFilled;
    }
    if (text == "FILLED") {
        return WireOrderStatus::Filled;
    }
    // futures-only and terminal; its fill was reported by its trade
    if (text == "CANCELED" || text == "PARTIALLY_CANCELED") {
        return WireOrderStatus::Cancelled;
    }
    if (text == "REJECTED") {
        return WireOrderStatus::Rejected;
    }
    if (text == "EXPIRED") {
        return WireOrderStatus::Expired;
    }
    return WireOrderStatus::Unknown;
}

std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFuturesOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WireFuturesOrder>(
            *array, storage.orders, [](auto& obj, WireFuturesOrder& row) {
                return read_futures_order(obj, row);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireFuturesOrder>{storage.orders.data(), *count};
    });
}

std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireFuturesOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto payload = payload_of(*root);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        WireFuturesOrder out{};
        if (const auto ok = read_futures_order(*payload, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFuturesDeal>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count =
            read_rows<WireFuturesDeal>(*array, storage.deals, [](auto& obj, WireFuturesDeal& row) {
                return read_futures_deal(obj, row);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireFuturesDeal>{storage.deals.data(), *count};
    });
}

std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFuturesDeal, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto payload = payload_of(*root);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        WireFuturesDeal out{};
        if (const auto ok = read_futures_deal(*payload, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

std::expected<std::string_view, WireError> parse_order_ack(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        wire::ondemand::value result;
        if (const auto ec = wire::find(*root, "result").get(result); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        wire::ondemand::json_type type{};
        if (const auto ec = result.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        // futures create/cancel answer a bare string; amend and spot wrap it as orderId or cancelId
        if (type != wire::ondemand::json_type::object) {
            return scalar_of(result);
        }
        wire::ondemand::object obj;
        if (const auto ec = result.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto order_id = opt_scalar(obj, "orderId");
        if (!order_id.has_value()) {
            return std::unexpected(order_id.error());
        }
        if (!order_id->empty()) {
            return *order_id;
        }
        return opt_scalar(obj, "cancelId");
    });
}

std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count =
            read_rows<WireSpotOrder>(*array, storage.orders, [](auto& obj, WireSpotOrder& row) {
                return read_spot_order(obj, row);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireSpotOrder>{storage.orders.data(), *count};
    });
}

std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireSpotOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto payload = payload_of(*root);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        WireSpotOrder out{};
        if (const auto ok = read_spot_order(*payload, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotTrade>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count =
            read_rows<WireSpotTrade>(*array, storage.trades, [](auto& obj, WireSpotTrade& row) {
                return read_spot_trade(obj, row);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireSpotTrade>{storage.trades.data(), *count};
    });
}

// two-letter keys: oq is ordered, eq executed
std::expected<WireSpotOrder, WireError> parse_spot_order_push(std::string_view frame) {
    return guarded([&]() -> std::expected<WireSpotOrder, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireSpotOrder out{};
        for (const auto& [key, field] : {std::pair{"s", &out.symbol},
                                         std::pair{"i", &out.order_id},
                                         std::pair{"ci", &out.client_order_id},
                                         std::pair{"sd", &out.side},
                                         std::pair{"tp", &out.type},
                                         std::pair{"oq", &out.orig_qty},
                                         std::pair{"eq", &out.executed_qty},
                                         std::pair{"lq", &out.leaving_qty},
                                         std::pair{"p", &out.price},
                                         std::pair{"ap", &out.avg_price},
                                         std::pair{"f", &out.fee}}) {
            if (const auto ok = take(*data, key, *field); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
        }
        if (const auto ok = take_status(*data, "st", out.state); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        const auto ts = opt_ms(*data, "t");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ms = *ts;
        if (out.symbol.empty() || out.order_id.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<WireSpotTrade, WireError> parse_spot_trade_push(std::string_view frame) {
    return guarded([&]() -> std::expected<WireSpotTrade, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireSpotTrade out{};
        // i is the trade id and oi the order id here, opposite of the order push; ids exceed double precision
        for (const auto& [key, field] : {std::pair{"s", &out.symbol},
                                         std::pair{"i", &out.trade_id},
                                         std::pair{"oi", &out.order_id},
                                         std::pair{"p", &out.price},
                                         std::pair{"q", &out.quantity}}) {
            if (const auto ok = take(*data, key, *field); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
        }
        // tm is the account's side, 1 taker 2 maker; b is about the print's buyer
        const auto liquidity = opt_i64(*data, "tm", 0);
        if (!liquidity.has_value()) {
            return std::unexpected(liquidity.error());
        }
        if (*liquidity == 1 || *liquidity == 2) {
            out.maker = *liquidity == 2;
            out.liquidity_known = true;
        }
        const auto ts = opt_ms(*data, "t");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ms = *ts;
        if (out.symbol.empty() || out.order_id.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

} // namespace raikiri::venues::xt
