#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::mexc {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;
namespace ondemand = detail::ondemand;

std::expected<void, WireError> read_futures_order(ondemand::object& obj, WireFuturesOrder& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    // `orderId` over REST, `id` over the socket. Neither host sends both.
    out.order_id = field(obj, "orderId");
    if (out.order_id.empty()) {
        out.order_id = field(obj, "id");
    }
    out.external_oid = field(obj, "externalOid");
    out.price = field(obj, "price");
    out.vol = field(obj, "vol");
    out.deal_vol = field(obj, "dealVol");
    out.deal_avg_price = field(obj, "dealAvgPrice");
    out.side = int_field(obj, "side");
    out.order_type = int_field(obj, "orderType");
    out.state = int_field(obj, "state");
    out.error_code = int_field(obj, "errorCode");
    out.update_ms = detail::digits_to_u64(field(obj, "updateTime"));
    if (out.update_ms == 0) {
        out.update_ms = detail::digits_to_u64(field(obj, "createTime"));
    }
    return {};
}

std::expected<void, WireError> read_futures_deal(ondemand::object& obj, WireFuturesDeal& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.deal_id = field(obj, "id");
    out.order_id = field(obj, "orderId");
    out.price = field(obj, "price");
    out.vol = field(obj, "vol");
    out.fee = field(obj, "fee");
    out.side = int_field(obj, "side");
    out.ts_ms = detail::digits_to_u64(field(obj, "timestamp"));

    obj.reset();
    if (const auto taker = detail::opt_bool(obj, "taker"); taker.has_value()) {
        out.taker = *taker;
        out.liquidity_known = true;
    }
    if (!out.liquidity_known) {
        obj.reset();
        if (const auto taker = detail::opt_bool(obj, "isTaker"); taker.has_value()) {
            out.taker = *taker;
            out.liquidity_known = true;
        }
    }
    return {};
}

std::expected<void, WireError> read_spot_order(ondemand::object& obj, WireSpotOrder& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.order_id = field(obj, "orderId");
    out.client_order_id = field(obj, "clientOrderId");
    out.price = field(obj, "price");
    out.orig_qty = field(obj, "origQty");
    out.executed_qty = field(obj, "executedQty");
    out.side = field(obj, "side");
    out.type = field(obj, "type");
    out.status = spot_order_status(field(obj, "status"));
    out.update_ms = detail::digits_to_u64(field(obj, "updateTime"));
    if (out.update_ms == 0) {
        // A create response has `transactTime` and neither of the other two.
        out.update_ms = detail::digits_to_u64(field(obj, "transactTime"));
    }
    if (out.update_ms == 0) {
        out.update_ms = detail::digits_to_u64(field(obj, "time"));
    }
    return {};
}

std::expected<void, WireError> read_spot_trade(ondemand::object& obj, WireSpotTrade& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.trade_id = field(obj, "id");
    out.order_id = field(obj, "orderId");
    out.client_order_id = field(obj, "clientOrderId");
    out.price = field(obj, "price");
    out.qty = field(obj, "qty");
    out.commission = field(obj, "commission");
    out.ts_ms = detail::digits_to_u64(field(obj, "time"));
    obj.reset();
    out.is_buyer = detail::opt_bool(obj, "isBuyer").value_or(false);
    obj.reset();
    out.is_maker = detail::opt_bool(obj, "isMaker").value_or(false);
    return {};
}

} // namespace

WireOrderStatus spot_order_status(std::string_view text) noexcept {
    if (text == "NEW") {
        return WireOrderStatus::New;
    }
    if (text == "PARTIALLY_FILLED") {
        return WireOrderStatus::PartiallyFilled;
    }
    if (text == "FILLED") {
        return WireOrderStatus::Filled;
    }
    // PARTIALLY_CANCELED is a cancel that took some volume first; the fill was
    // already reported by its own trade, so the lifecycle event is a cancel.
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
    return guarded([&] {
        return read_envelope_array<WireFuturesOrder>(body, storage.orders, read_futures_order);
    });
}

std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body) {
    return guarded(
        [&] { return read_envelope_object<WireFuturesOrder>(body, read_futures_order); });
}

std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage) {
    return guarded([&] {
        return read_envelope_array<WireFuturesDeal>(body, storage.deals, read_futures_deal);
    });
}

std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame) {
    return guarded([&] { return read_envelope_object<WireFuturesDeal>(frame, read_futures_deal); });
}

std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage) {
    return guarded(
        [&] { return read_bare_array<WireSpotOrder>(body, storage.orders, read_spot_order); });
}

std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireSpotOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireSpotOrder out{};
        if (const auto ok = read_spot_order(*root, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage) {
    return guarded(
        [&] { return read_bare_array<WireSpotTrade>(body, storage.trades, read_spot_trade); });
}

} // namespace raikiri::venues::mexc
