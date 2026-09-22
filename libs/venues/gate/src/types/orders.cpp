#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::gate {

namespace {

using wire::guarded;
using wire::session;
namespace ondemand = wire::ondemand;

// Venue milliseconds from whichever of Gate's six timestamp spellings this
// object carries. The `_ms` fields are milliseconds; the bare ones are
// seconds, sometimes with a fractional tail. Reading one as the other puts the
// event in 1970 or 30 millennia out, and both are silent.
[[nodiscard]] std::uint64_t timestamp_ms(ondemand::object& obj) {
    for (const auto key : {"update_time_ms", "finish_time_ms", "create_time_ms"}) {
        if (const auto ms = whole_u64(field(obj, key)); ms != 0) {
            return ms;
        }
    }
    for (const auto key : {"update_time", "finish_time", "create_time"}) {
        if (const auto s = whole_u64(field(obj, key)); s != 0) {
            return s * 1'000ULL;
        }
    }
    return 0;
}

[[nodiscard]] WireOrderState terminal_state(std::string_view finish_as,
                                            bool left_is_zero) noexcept {
    if (finish_as == "filled") {
        return WireOrderState::Filled;
    }
    // A tif that could not be met is an expiry, not a cancel: nobody asked for
    // the order to go away, the venue could not honour its terms.
    if (finish_as == "ioc" || finish_as == "poc" || finish_as == "fok") {
        return WireOrderState::Expired;
    }
    // Spot spells "no reason given" as a bare hyphen on the socket and as
    // "unknown" over REST; both mean the remaining size is the only evidence.
    if (finish_as.empty() || finish_as == "unknown" || finish_as == "-") {
        return left_is_zero ? WireOrderState::Filled : WireOrderState::Cancelled;
    }
    return WireOrderState::Cancelled;
}

// `_new` and `_update` are websocket-only pseudo-values that never appear on a
// REST body: reading them as terminal reasons closes an order that is very
// much still working.
[[nodiscard]] WireOrderState futures_state(std::string_view status,
                                           std::string_view finish_as,
                                           bool left_is_zero,
                                           bool& accepted) noexcept {
    if (finish_as == "_new") {
        accepted = true;
        return WireOrderState::Open;
    }
    if (finish_as == "_update") {
        return WireOrderState::Open;
    }
    if (!finish_as.empty()) {
        return terminal_state(finish_as, left_is_zero);
    }
    if (status == "open") {
        accepted = true;
        return WireOrderState::Open;
    }
    if (status == "finished") {
        return terminal_state({}, left_is_zero);
    }
    return WireOrderState::Unknown;
}

[[nodiscard]] WireOrderState spot_state(std::string_view status,
                                        std::string_view event,
                                        std::string_view finish_as,
                                        bool left_is_zero,
                                        bool& accepted) noexcept {
    if (event == "put") {
        accepted = true;
        return WireOrderState::Open;
    }
    if (event == "update") {
        return WireOrderState::Open;
    }
    if (event == "finish") {
        return terminal_state(finish_as, left_is_zero);
    }
    if (status == "open") {
        accepted = true;
        return WireOrderState::Open;
    }
    if (status == "closed") {
        return WireOrderState::Filled;
    }
    if (status == "cancelled") {
        return WireOrderState::Cancelled;
    }
    return WireOrderState::Unknown;
}

[[nodiscard]] bool is_zero_qty(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c != '0' && c != '.' && c != '-' && c != '+') {
            return false;
        }
    }
    return true;
}

std::expected<void, WireError> read_futures_order(ondemand::object& obj, WireOrder& out) {
    const auto contract = wire::req_str(obj, "contract");
    if (!contract.has_value()) {
        return std::unexpected{contract.error()};
    }
    out.contract = *contract;
    out.order_id = field(obj, "id");
    if (out.order_id.empty()) {
        return std::unexpected{WireError::MissingField};
    }
    out.text = text_field(obj, "text");
    out.price = field(obj, "price");
    out.fill_price = field(obj, "fill_price");
    out.tif = text_field(obj, "tif");

    const auto size = split_sign(field(obj, "size"));
    const auto left = split_sign(field(obj, "left"));
    out.amount = size.magnitude;
    out.left = left.magnitude;
    out.sell = size.negative;

    out.ts_ms = timestamp_ms(obj);
    out.state = futures_state(text_field(obj, "status"),
                              text_field(obj, "finish_as"),
                              is_zero_qty(out.left),
                              out.accepted);
    return {};
}

std::expected<void, WireError> read_spot_order(ondemand::object& obj, WireOrder& out) {
    const auto pair = wire::req_str(obj, "currency_pair");
    if (!pair.has_value()) {
        return std::unexpected{pair.error()};
    }
    out.contract = *pair;
    // Required, and load-bearing beyond identity: /spot/open_orders wraps its
    // orders in objects that also carry `currency_pair` but no `id`, so a
    // reader that tolerates a missing one reads the WRAPPERS as orders and
    // reports a set of empty ones instead of failing.
    out.order_id = field(obj, "id");
    if (out.order_id.empty()) {
        return std::unexpected{WireError::MissingField};
    }
    out.text = text_field(obj, "text");
    out.price = field(obj, "price");
    out.fill_price = field(obj, "avg_deal_price");
    out.tif = text_field(obj, "time_in_force");
    out.order_type = text_field(obj, "type");
    out.amount = field(obj, "amount");
    out.left = field(obj, "left");
    out.sell = text_field(obj, "side") == "sell";
    out.ts_ms = timestamp_ms(obj);
    out.state = spot_state(text_field(obj, "status"),
                           text_field(obj, "event"),
                           text_field(obj, "finish_as"),
                           is_zero_qty(out.left),
                           out.accepted);
    return {};
}

std::expected<void, WireError>
read_order(ondemand::object& obj, WireOrder& out, MarketType market) {
    switch (market) {
    case MarketType::Perp:
        return read_futures_order(obj, out);
    case MarketType::Spot:
        return read_spot_order(obj, out);
    case MarketType::Future:
    case MarketType::Option:
        // Delivery and options have their own bodies; nothing subscribes to
        // them, so a frame arriving under one is a routing bug, not data.
        return std::unexpected{WireError::UnknownMessage};
    }
    return std::unexpected{WireError::UnknownMessage};
}

std::expected<void, WireError>
read_user_trade(ondemand::object& obj, WireUserTrade& out, MarketType market) {
    switch (market) {
    case MarketType::Perp: {
        const auto contract = wire::req_str(obj, "contract");
        if (!contract.has_value()) {
            return std::unexpected{contract.error()};
        }
        out.contract = *contract;
        const auto size = split_sign(field(obj, "size"));
        out.qty = size.magnitude;
        out.sell = size.negative;
        break;
    }
    case MarketType::Spot: {
        const auto pair = wire::req_str(obj, "currency_pair");
        if (!pair.has_value()) {
            return std::unexpected{pair.error()};
        }
        out.contract = *pair;
        out.qty = field(obj, "amount");
        out.sell = text_field(obj, "side") == "sell";
        break;
    }
    case MarketType::Future:
    case MarketType::Option:
        return std::unexpected{WireError::UnknownMessage};
    }
    out.trade_id = field(obj, "id");
    out.order_id = field(obj, "order_id");
    out.text = text_field(obj, "text");
    out.price = field(obj, "price");
    out.fee = field(obj, "fee");
    out.maker = text_field(obj, "role") == "maker";
    out.ts_ms = timestamp_ms(obj);
    return {};
}

} // namespace

std::expected<std::span<const WireOrder>, WireError>
parse_ws_orders(std::string_view frame, OrderBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_array(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        return read_array<WireOrder>(
            *result, storage.orders, [market](ondemand::object& obj, WireOrder& out) {
                return read_order(obj, out, market);
            });
    });
}

std::expected<std::span<const WireUserTrade>, WireError>
parse_ws_user_trades(std::string_view frame, UserTradeBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<std::span<const WireUserTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_array(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        return read_array<WireUserTrade>(
            *result, storage.trades, [market](ondemand::object& obj, WireUserTrade& out) {
                return read_user_trade(obj, out, market);
            });
    });
}

std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_array<WireOrder>(
            *root, storage.orders, [market](ondemand::object& obj, WireOrder& out) {
                return read_order(obj, out, market);
            });
    });
}

std::expected<WireOrder, WireError> parse_rest_order(std::string_view body, MarketType market) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireOrder out{};
        if (auto ok = read_order(*root, out, market); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_spot_open_orders(std::string_view body,
                                                                            OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto group : *root) {
            ondemand::object obj;
            if (group.get_object().get(obj) != simdjson::SUCCESS) {
                return std::unexpected(WireError::WrongType);
            }
            auto orders = wire::req_array(obj, "orders");
            if (!orders.has_value()) {
                return std::unexpected(orders.error());
            }
            for (auto item : *orders) {
                if (count == storage.orders.size()) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                ondemand::object row;
                if (item.get_object().get(row) != simdjson::SUCCESS) {
                    return std::unexpected(WireError::WrongType);
                }
                WireOrder out{};
                if (auto ok = read_spot_order(row, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                storage.orders[count] = out;
                ++count;
            }
        }
        return std::span<const WireOrder>{storage.orders.data(), count};
    });
}

std::expected<std::span<const WireUserTrade>, WireError>
parse_rest_user_trades(std::string_view body, UserTradeBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<std::span<const WireUserTrade>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_array<WireUserTrade>(
            *root, storage.trades, [market](ondemand::object& obj, WireUserTrade& out) {
                return read_user_trade(obj, out, market);
            });
    });
}

} // namespace raikiri::venues::gate
