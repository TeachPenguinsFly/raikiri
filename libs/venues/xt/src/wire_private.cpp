#include "wire_detail.hpp"

namespace raikiri::venues::xt {

namespace {

using detail::guarded;
using detail::session;

// `result` is an ARRAY on the unpaged reads (/v4/open-order, position/list) and
// an OBJECT wrapping `items` on the paged ones (/v4/trade, order/trade-list).
// Asking for one shape and getting the other is not an error XT reports — the
// read simply returns nothing — so the shape is resolved before the walk.
std::expected<detail::ondemand::array, WireError> rows_of(detail::ondemand::object& root,
                                                          std::string_view key) {
    detail::ondemand::value result;
    if (const auto ec = root.find_field_unordered("result").get(result); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    detail::ondemand::json_type type{};
    if (const auto ec = result.type().get(type); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    if (type == detail::ondemand::json_type::array) {
        detail::ondemand::array array;
        if (const auto ec = result.get_array().get(array); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        return array;
    }
    if (type != detail::ondemand::json_type::object) {
        return std::unexpected(WireError::WrongType);
    }
    detail::ondemand::object page;
    if (const auto ec = result.get_object().get(page); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return detail::req_array(page, key);
}

template <typename Row, typename Read>
std::expected<std::size_t, WireError>
read_rows(detail::ondemand::array array, std::span<Row> into, Read read) {
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        Row row{};
        if (const auto ok = read(obj, row); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = row;
        ++count;
    }
    return count;
}

// The single-record reads put the payload under `result`; the socket puts it
// under `data`. Nothing else distinguishes them, so both are tried.
std::expected<detail::ondemand::object, WireError> payload_of(detail::ondemand::object& root) {
    if (detail::has_field(root, "result")) {
        return detail::req_object(root, "result");
    }
    return detail::req_object(root, "data");
}

// Copies every decimal-or-id field with detail::opt_scalar, because XT ships
// the SAME field in both encodings depending on the endpoint: /order/detail
// quotes nothing and /order/list-open-order quotes everything.
std::expected<void, WireError>
take(detail::ondemand::object& obj, std::string_view key, std::string_view& into) {
    const auto value = detail::opt_scalar(obj, key);
    if (!value.has_value()) {
        return std::unexpected(value.error());
    }
    into = *value;
    return {};
}

std::expected<void, WireError>
take_status(detail::ondemand::object& obj, std::string_view key, WireOrderStatus& into) {
    const auto text = detail::opt_scalar(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    into = order_status(*text);
    return {};
}

// XT's `takerMaker` is TEXT and the two trees disagree about its case:
// /order/trade-list says "TAKER" and /v4/trade says "taker".
bool text_is_maker(std::string_view text) noexcept {
    return text.size() == 5 && (text[0] == 'M' || text[0] == 'm');
}

std::expected<void, WireError> read_futures_order(detail::ondemand::object& obj,
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
    const auto created = detail::opt_ms(obj, "createdTime");
    const auto updated = detail::opt_ms(obj, "updatedTime");
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

std::expected<void, WireError> read_futures_deal(detail::ondemand::object& obj,
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
    // Two transports, two spellings, and neither sends both: REST states
    // `takerMaker` as text, the socket states `isMaker` as a boolean.
    const auto taker_maker = detail::opt_scalar(obj, "takerMaker");
    if (!taker_maker.has_value()) {
        return std::unexpected(taker_maker.error());
    }
    if (!taker_maker->empty()) {
        out.maker = text_is_maker(*taker_maker);
        out.liquidity_known = true;
    } else if (detail::has_field(obj, "isMaker")) {
        const auto is_maker = detail::opt_bool_or(obj, "isMaker", false);
        if (!is_maker.has_value()) {
            return std::unexpected(is_maker.error());
        }
        out.maker = *is_maker;
        out.liquidity_known = true;
    }
    // `timestamp` on both transports; the REST page repeats it as `time` on
    // some tenants, so the second spelling is a fallback rather than a schema.
    const auto ts = detail::opt_ms(obj, "timestamp");
    if (!ts.has_value()) {
        return std::unexpected(ts.error());
    }
    out.ts_ms = *ts;
    if (out.ts_ms == 0) {
        const auto alt = detail::opt_ms(obj, "time");
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

std::expected<void, WireError> read_futures_balance(detail::ondemand::object& obj,
                                                    WireFuturesBalance& out) {
    for (const auto& [key, field] : {std::pair{"coin", &out.coin},
                                     std::pair{"walletBalance", &out.wallet_balance},
                                     std::pair{"availableBalance", &out.available_balance}}) {
        if (const auto ok = take(obj, key, *field); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
    }
    return {};
}

std::expected<void, WireError> read_spot_order(detail::ondemand::object& obj, WireSpotOrder& out) {
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
    // `updatedTime` is when the order last moved and `time` is when it was
    // created; the first is the one a lifecycle event is stamped with.
    const auto updated = detail::opt_ms(obj, "updatedTime");
    const auto created = detail::opt_ms(obj, "time");
    if (!updated.has_value() || !created.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.ts_ms = *updated != 0 ? *updated : *created;
    if (out.symbol.empty() || out.order_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<void, WireError> read_spot_trade(detail::ondemand::object& obj, WireSpotTrade& out) {
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
    const auto taker_maker = detail::opt_scalar(obj, "takerMaker");
    if (!taker_maker.has_value()) {
        return std::unexpected(taker_maker.error());
    }
    if (!taker_maker->empty()) {
        out.maker = text_is_maker(*taker_maker);
        out.liquidity_known = true;
    }
    const auto ts = detail::opt_ms(obj, "time");
    if (!ts.has_value()) {
        return std::unexpected(ts.error());
    }
    out.ts_ms = *ts;
    if (out.symbol.empty() || out.order_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

PrivateTopic topic_of(std::string_view topic) noexcept {
    if (topic == "order") {
        return PrivateTopic::Order;
    }
    if (topic == "trade") {
        return PrivateTopic::Trade;
    }
    if (topic == "position") {
        return PrivateTopic::Position;
    }
    if (topic == "balance") {
        return PrivateTopic::Balance;
    }
    return PrivateTopic::Unknown;
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
    // PARTIALLY_CANCELED exists on futures only and is terminal: the volume it
    // took was already reported by its own trade, so what is left to say is
    // that the rest was cancelled.
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

std::expected<std::span<const WireFuturesBalance>, WireError>
parse_futures_balances(std::string_view body, FuturesBalanceBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFuturesBalance>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = rows_of(*root, "items");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        // A coinless row is kept here and dropped by the mapper: this reader
        // reports what XT sent, and the decision that a number with no unit is
        // not a balance belongs where the domain type is built.
        const auto count = read_rows<WireFuturesBalance>(
            *array, storage.balances, [](auto& obj, WireFuturesBalance& row) {
                return read_futures_balance(obj, row);
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireFuturesBalance>{storage.balances.data(), *count};
    });
}

std::expected<std::string_view, WireError> parse_order_ack(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        detail::ondemand::value result;
        if (const auto ec = root->find_field_unordered("result").get(result);
            ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        detail::ondemand::json_type type{};
        if (const auto ec = result.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        // Futures create and cancel answer with the id as a BARE STRING;
        // futures amend and every spot write wrap it in an object, under
        // `orderId` on a create or amend and `cancelId` on a cancel.
        if (type != detail::ondemand::json_type::object) {
            return detail::scalar_of(result);
        }
        detail::ondemand::object obj;
        if (const auto ec = result.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto order_id = detail::opt_scalar(obj, "orderId");
        if (!order_id.has_value()) {
            return std::unexpected(order_id.error());
        }
        if (!order_id->empty()) {
            return *order_id;
        }
        return detail::opt_scalar(obj, "cancelId");
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

// The two-letter set. A reader that guessed between this and the REST spelling
// would read `oq` (ordered) where it meant `eq` (executed) and report an
// untouched order as fully filled.
std::expected<WireSpotOrder, WireError> parse_spot_order_push(std::string_view frame) {
    return guarded([&]() -> std::expected<WireSpotOrder, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
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
        const auto ts = detail::opt_ms(*data, "t");
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
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireSpotTrade out{};
        // `i` is the TRADE id here and `oi` the order id — the opposite of the
        // order push, where `i` is the order. The ids are JSON numbers wider
        // than a double holds exactly, which is why the token is kept as text.
        for (const auto& [key, field] : {std::pair{"s", &out.symbol},
                                         std::pair{"i", &out.trade_id},
                                         std::pair{"oi", &out.order_id},
                                         std::pair{"p", &out.price},
                                         std::pair{"q", &out.quantity}}) {
            if (const auto ok = take(*data, key, *field); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
        }
        // `tm` is the ACCOUNT's side of the book: 1 taker, 2 maker. `b` beside
        // it says whether the BUYER was the maker, which is a fact about the
        // print rather than about us, and reading it as ours inverts the tag.
        const auto liquidity = detail::opt_i64(*data, "tm", 0);
        if (!liquidity.has_value()) {
            return std::unexpected(liquidity.error());
        }
        if (*liquidity == 1 || *liquidity == 2) {
            out.maker = *liquidity == 2;
            out.liquidity_known = true;
        }
        const auto ts = detail::opt_ms(*data, "t");
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

std::expected<SpotEnvelope, WireError> parse_spot_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<SpotEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto rc = detail::req_i64(*root, "rc");
        const auto mc = detail::opt_scalar(*root, "mc");
        if (!rc.has_value() || !mc.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return SpotEnvelope{.rc = *rc, .mc = *mc};
    });
}

std::expected<std::string_view, WireError> parse_spot_ws_token(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = detail::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto token = detail::opt_scalar(*result, "accessToken");
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        if (token->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return *token;
    });
}

std::expected<std::string_view, WireError> parse_listen_key(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        detail::ondemand::value result;
        if (const auto ec = root->find_field_unordered("result").get(result);
            ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        detail::ondemand::json_type type{};
        if (const auto ec = result.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        // `result` is the key STRING itself. XT's own reference prints it as
        // `{}` — that is the doc's placeholder for every result, not a shape —
        // so an object is still read, for the one field it could hold.
        if (type != detail::ondemand::json_type::object) {
            const auto key = detail::scalar_of(result);
            if (!key.has_value()) {
                return std::unexpected(key.error());
            }
            if (key->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            return *key;
        }
        detail::ondemand::object obj;
        if (const auto ec = result.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto key = detail::opt_scalar(obj, "listenKey");
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
        if (key->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return *key;
    });
}

std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = detail::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto ms = detail::opt_ms(*result, "serverTime");
        if (!ms.has_value()) {
            return std::unexpected(ms.error());
        }
        if (*ms == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ms;
    });
}

std::expected<PrivateFrame, WireError> parse_private_frame(std::string_view frame) {
    return guarded([&]() -> std::expected<PrivateFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (detail::has_field(*root, "topic")) {
            const auto topic = detail::req_str(*root, "topic");
            if (!topic.has_value()) {
                return std::unexpected(topic.error());
            }
            return PrivateFrame{.topic = topic_of(*topic), .code = 0, .msg = {}};
        }
        // The subscribe answer, identical in shape on both trees:
        // 0 accepted, 1 refused, 2 the listen key is no longer valid.
        const auto code = detail::opt_i64(*root, "code", -1);
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        if (*code < 0) {
            return std::unexpected(WireError::UnknownMessage);
        }
        const auto msg = detail::opt_scalar(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return PrivateFrame{.topic = *code == k_ok ? PrivateTopic::Ack : PrivateTopic::Refusal,
                            .code = *code,
                            .msg = *msg};
    });
}

std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotSymbol>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = detail::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        auto array = detail::req_array(*result, "symbols");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_spot_symbols) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireSpotSymbol out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto state = detail::req_str(obj, "state");
            if (!symbol.has_value() || !state.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.state = *state;
            const auto price_precision = detail::req_i64(obj, "pricePrecision");
            const auto qty_precision = detail::req_i64(obj, "quantityPrecision");
            const auto trading = detail::req_bool(obj, "tradingEnabled");
            const auto openapi = detail::req_bool(obj, "openapiEnabled");
            if (!price_precision.has_value() || !qty_precision.has_value() ||
                !trading.has_value() || !openapi.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price_precision = *price_precision;
            out.qty_precision = *qty_precision;
            out.trading_enabled = *trading;
            out.openapi_enabled = *openapi;
            // `filters` is a heterogeneous list keyed by a `filter` tag, so the
            // minimum notional is found by walking it rather than by name.
            if (auto filters = detail::req_array(obj, "filters"); filters.has_value()) {
                for (auto filter : *filters) {
                    detail::ondemand::object row;
                    if (const auto ec = filter.get_object().get(row); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    const auto tag = detail::opt_scalar(row, "filter");
                    if (!tag.has_value()) {
                        return std::unexpected(tag.error());
                    }
                    if (*tag != "QUOTE_QTY") {
                        continue;
                    }
                    const auto min = detail::opt_scalar(row, "min");
                    if (!min.has_value()) {
                        return std::unexpected(min.error());
                    }
                    out.min_quote_qty = *min;
                    break;
                }
            }
            storage.symbols[count] = out;
            ++count;
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), count};
    });
}

} // namespace raikiri::venues::xt
