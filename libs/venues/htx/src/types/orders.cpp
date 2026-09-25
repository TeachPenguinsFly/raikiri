#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

namespace {

using wire::guarded;
using wire::session;

// swap order ids exceed 2^53, so read order_id_str first
std::expected<std::string_view, WireError> order_id_of(wire::ondemand::object& obj) {
    const auto text = wire::opt_str(obj, "order_id_str");
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    if (!text->empty()) {
        return *text;
    }
    return num_of(obj, "order_id");
}

// 1, 2 and 11 are transient states and produce no event
WireOrderState swap_state(std::int64_t status) noexcept {
    switch (status) {
    case 3:
        return WireOrderState::Accepted;
    case 4:
        return WireOrderState::PartiallyFilled;
    case 5: // partially filled, then cancelled: finished, not still working
    case 7:
        return WireOrderState::Canceled;
    case 6:
        return WireOrderState::Filled;
    default:
        return WireOrderState::Unknown;
    }
}

WireOrderState spot_state(std::string_view status) noexcept {
    if (status == "submitted" || status == "created") {
        return WireOrderState::Accepted;
    }
    if (status == "partial-filled") {
        return WireOrderState::PartiallyFilled;
    }
    if (status == "filled") {
        return WireOrderState::Filled;
    }
    if (status == "canceled" || status == "partial-canceled") {
        return WireOrderState::Canceled;
    }
    if (status == "rejected") {
        return WireOrderState::Rejected;
    }
    // "canceling" and anything unrecognised: in flight, not a verdict.
    return WireOrderState::Unknown;
}

// role is lowercase on the socket and capitalised in rest rows
bool is_maker(std::string_view role) noexcept {
    constexpr std::string_view k_maker = "maker";
    if (role.size() != k_maker.size()) {
        return false;
    }
    for (std::size_t i = 0; i < role.size(); ++i) {
        const char c = role[i] >= 'A' && role[i] <= 'Z' ? static_cast<char>(role[i] - 'A' + 'a') : role[i];
        if (c != k_maker[i]) {
            return false;
        }
    }
    return true;
}

bool spot_sells(std::string_view type) noexcept {
    return type.starts_with("sell");
}

std::expected<std::size_t, WireError> read_swap_fills(wire::ondemand::object& obj, std::span<WireFill> into) {
    if (into.empty()) {
        return std::size_t{0}; // a caller that asked for no fills wants none
    }
    wire::ondemand::array trades;
    if (const auto ec = wire::find(obj, "trade").get_array().get(trades); ec != simdjson::SUCCESS) {
        return std::size_t{0}; // no trades on this frame is the ordinary case
    }
    std::size_t count = 0;
    for (auto entry : trades) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object trade;
        if (const auto ec = entry.get_object().get(trade); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto id = wire::opt_str(trade, "id");
        const auto price = num_of(trade, "trade_price");
        const auto qty = num_of(trade, "trade_volume");
        const auto fee = num_of(trade, "trade_fee");
        const auto role = wire::opt_str(trade, "role");
        const auto created = wire::opt_u64(trade, "created_at");
        if (!id.has_value() || !price.has_value() || !qty.has_value() || !fee.has_value() || !role.has_value() ||
            !created.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        auto& out = into[count];
        out.exec_id = *id;
        if (out.exec_id.empty()) {
            // id is globally unique; trade_id only per contract
            const auto fallback = num_of(trade, "trade_id");
            if (!fallback.has_value()) {
                return std::unexpected(fallback.error());
            }
            out.exec_id = *fallback;
        }
        out.price = *price;
        out.qty = *qty;
        out.fee = *fee;
        out.maker = is_maker(*role);
        out.ts_ms = *created;
        ++count;
    }
    return count;
}

std::expected<WireOrder, WireError> read_swap_order(wire::ondemand::object& obj, std::span<WireFill> fills) {
    WireOrder out{};
    const auto code = wire::req_str(obj, "contract_code");
    const auto order_id = order_id_of(obj);
    const auto client_id = num_of(obj, "client_order_id");
    const auto price = num_of(obj, "price");
    const auto volume = num_of(obj, "volume");
    const auto traded = num_of(obj, "trade_volume");
    const auto direction = wire::opt_str(obj, "direction");
    const auto price_type = wire::opt_str(obj, "order_price_type");
    const auto status = wire::opt_i64(obj, "status");
    const auto created = wire::opt_u64(obj, "created_at");
    if (!code.has_value() || !order_id.has_value() || !client_id.has_value() || !price.has_value() ||
        !volume.has_value() || !traded.has_value() || !direction.has_value() || !price_type.has_value() ||
        !status.has_value() || !created.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.contract_code = *code;
    out.order_id = *order_id;
    out.client_order_id = *client_id;
    out.price = *price;
    out.qty = *volume;
    out.filled_qty = *traded;
    out.order_price_type = *price_type;
    out.sell = *direction == "sell";
    out.state = swap_state(*status);
    out.ts_ms = *created;

    const auto count = read_swap_fills(obj, fills);
    if (!count.has_value()) {
        return std::unexpected(count.error());
    }
    out.fills = std::span<const WireFill>{fills.data(), *count};
    return out;
}

// reads both the websocket's camelCase and rest's hyphenated fields
std::expected<WireOrder, WireError> read_spot_order(wire::ondemand::object& obj) {
    WireOrder out{};
    const auto symbol = wire::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected(symbol.error());
    }
    out.contract_code = *symbol;

    const auto ws_order_id = num_of(obj, "orderId");
    const auto rest_order_id = num_of(obj, "id");
    const auto ws_client = wire::opt_str(obj, "clientOrderId");
    const auto rest_client = wire::opt_str(obj, "client-order-id");
    const auto ws_price = num_of(obj, "orderPrice");
    const auto rest_price = num_of(obj, "price");
    const auto ws_size = num_of(obj, "orderSize");
    const auto rest_size = num_of(obj, "amount");
    const auto left = num_of(obj, "remainAmt");
    const auto ws_exec = num_of(obj, "execAmt");
    // "field-amount" is htx's misspelling, still answered by some spot endpoints
    const auto rest_exec = num_of(obj, "filled-amount");
    const auto legacy_exec = num_of(obj, "field-amount");
    const auto type = wire::opt_str(obj, "type");
    const auto ws_status = wire::opt_str(obj, "orderStatus");
    const auto rest_status = wire::opt_str(obj, "state");
    const auto event = wire::opt_str(obj, "eventType");
    if (!ws_order_id.has_value() || !rest_order_id.has_value() || !ws_client.has_value() || !rest_client.has_value() ||
        !ws_price.has_value() || !rest_price.has_value() || !ws_size.has_value() || !rest_size.has_value() ||
        !left.has_value() || !ws_exec.has_value() || !rest_exec.has_value() || !legacy_exec.has_value() ||
        !type.has_value() || !ws_status.has_value() || !rest_status.has_value() || !event.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.order_id = ws_order_id->empty() ? *rest_order_id : *ws_order_id;
    out.client_order_id = ws_client->empty() ? *rest_client : *ws_client;
    out.price = ws_price->empty() ? *rest_price : *ws_price;
    out.qty = ws_size->empty() ? *rest_size : *ws_size;
    out.left_qty = *left;
    out.filled_qty = ws_exec->empty() ? (rest_exec->empty() ? *legacy_exec : *rest_exec) : *ws_exec;
    out.order_price_type = *type;
    out.sell = spot_sells(*type);

    const auto status = ws_status->empty() ? *rest_status : *ws_status;
    if (!status.empty()) {
        out.state = spot_state(status);
    } else if (*event == "creation") {
        // a creation push has no orderStatus; the event name is the state
        out.state = WireOrderState::Accepted;
    } else if (*event == "cancellation" || *event == "deletion") {
        out.state = WireOrderState::Canceled;
    }

    const auto created = wire::opt_u64(obj, "orderCreateTime");
    const auto acted = wire::opt_u64(obj, "lastActTime");
    const auto traded = wire::opt_u64(obj, "tradeTime");
    const auto rest_created = wire::opt_u64(obj, "created-at");
    if (!created.has_value() || !acted.has_value() || !traded.has_value() || !rest_created.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    for (const auto candidate : {*traded, *acted, *created, *rest_created}) {
        if (candidate != 0) {
            out.ts_ms = candidate;
            break;
        }
    }
    return out;
}

} // namespace

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body, MarketType market) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (market == MarketType::Spot) {
            // spot's data is the order id as a bare string
            const auto id = wire::opt_str(*root, "data");
            if (!id.has_value()) {
                return std::unexpected(id.error());
            }
            if (id->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            return WireOrderAck{.order_id = *id, .client_order_id = {}};
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto order_id = order_id_of(*data);
        const auto client_id = num_of(*data, "client_order_id");
        if (!order_id.has_value() || !client_id.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        if (order_id->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireOrderAck{.order_id = *order_id, .client_order_id = *client_id};
    });
}

std::expected<WireCancelOutcome, WireError> parse_cancel_outcome(std::string_view body, MarketType market) {
    return guarded([&]() -> std::expected<WireCancelOutcome, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        wire::ondemand::value data;
        if (const auto ec = wire::find(*root, "data").get(data); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        simdjson::ondemand::json_type type{};
        if (const auto ec = data.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (market == MarketType::Spot) {
            // cancel by client id answers an integer state; 0 means not found
            if (type == simdjson::ondemand::json_type::number) {
                std::int64_t state = 0;
                if (const auto ec = data.get_int64().get(state); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                return WireCancelOutcome{.accepted = state != 0,
                                         .code = state == 0 ? k_client_id_not_found : std::string_view{}};
            }
            return WireCancelOutcome{.accepted = true, .code = {}};
        }
        wire::ondemand::object obj;
        if (const auto ec = data.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        // status is ok either way; the verdict is in errors, and successes is a comma-joined string
        std::string_view first_error;
        if (wire::ondemand::array errors; wire::find(obj, "errors").get_array().get(errors) == simdjson::SUCCESS) {
            for (auto entry : errors) {
                wire::ondemand::object row;
                if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                const auto code = num_of(row, "err_code");
                if (!code.has_value()) {
                    return std::unexpected(code.error());
                }
                first_error = *code;
                break;
            }
        }
        const auto successes = wire::opt_str(obj, "successes");
        if (!successes.has_value()) {
            return std::unexpected(successes.error());
        }
        return WireCancelOutcome{.accepted = first_error.empty() && !successes->empty(), .code = first_error};
    });
}

std::expected<WireOrder, WireError> parse_swap_order_push(std::string_view frame, OrderBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // no envelope: the order's fields sit at the root
        return read_swap_order(*root, storage.fills);
    });
}

std::expected<WireOrder, WireError> parse_spot_order_push(std::string_view frame) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        return read_spot_order(*data);
    });
}

std::expected<WireUserTrade, WireError> parse_spot_trade_push(std::string_view frame) {
    return guarded([&]() -> std::expected<WireUserTrade, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto symbol = wire::req_str(*data, "symbol");
        const auto order_id = num_of(*data, "orderId");
        const auto client_id = wire::opt_str(*data, "clientOrderId");
        const auto price = num_of(*data, "tradePrice");
        const auto qty = num_of(*data, "tradeVolume");
        const auto trade_id = num_of(*data, "tradeId");
        const auto fee = num_of(*data, "transactFee");
        const auto side = wire::opt_str(*data, "orderSide");
        const auto type = wire::opt_str(*data, "orderType");
        const auto left = num_of(*data, "remainAmt");
        const auto ts = wire::opt_u64(*data, "tradeTime");
        // aggressor is a boolean taker flag here
        const auto aggressor = wire::opt_bool(*data, "aggressor");
        if (!symbol.has_value() || !order_id.has_value() || !client_id.has_value() || !price.has_value() ||
            !qty.has_value() || !trade_id.has_value() || !fee.has_value() || !side.has_value() || !type.has_value() ||
            !left.has_value() || !ts.has_value() || !aggressor.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireUserTrade out{};
        out.contract_code = *symbol;
        out.order_id = *order_id;
        out.client_order_id = *client_id;
        out.sell = side->empty() ? spot_sells(*type) : *side == "sell";
        out.left_qty = *left;
        out.fill = {
            .exec_id = *trade_id, .price = *price, .qty = *qty, .fee = *fee, .maker = !*aggressor, .ts_ms = *ts};
        return out;
    });
}

std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        const auto read_rows = [&](wire::ondemand::array rows) -> std::expected<std::span<const WireOrder>, WireError> {
            for (auto entry : rows) {
                if (count == k_max_orders) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                wire::ondemand::object obj;
                if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                const auto order = market == MarketType::Spot ? read_spot_order(obj)
                                                              : read_swap_order(obj, std::span<WireFill>{});
                if (!order.has_value()) {
                    return std::unexpected(order.error());
                }
                storage.orders[count] = *order;
                ++count;
            }
            return std::span<const WireOrder>{storage.orders.data(), count};
        };

        // data is an array, an object or a paging object by endpoint; check the type before reading
        wire::ondemand::value data;
        if (const auto ec = wire::find(*root, "data").get(data); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        simdjson::ondemand::json_type type{};
        if (const auto ec = data.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (type == simdjson::ondemand::json_type::object) {
            wire::ondemand::object obj;
            if (const auto ec = data.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            if (market == MarketType::Spot) {
                const auto order = read_spot_order(obj);
                if (!order.has_value()) {
                    return std::unexpected(order.error());
                }
                storage.orders[0] = *order;
                return std::span<const WireOrder>{storage.orders.data(), 1};
            }
            auto array = wire::req_array(obj, "orders");
            if (!array.has_value()) {
                return std::unexpected(array.error());
            }
            return read_rows(*array);
        }
        wire::ondemand::array rows;
        if (const auto ec = data.get_array().get(rows); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return read_rows(rows);
    });
}

} // namespace raikiri::venues::htx
