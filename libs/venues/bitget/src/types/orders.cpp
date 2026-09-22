#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitget {

namespace {

using wire::guarded;
using wire::session;
namespace ondemand = wire::ondemand;

// feeDetail has three shapes across the endpoints that carry it: an object on
// spot fills, an ARRAY of the same object on mix fills, and a JSON-encoded
// STRING on spot orderInfo. Only the first two are money we can read here.
std::expected<std::string_view, WireError> read_total_fee(ondemand::object& obj) {
    obj.reset();
    ondemand::value value;
    const auto ec = wire::find(obj, "feeDetail").get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    ondemand::json_type type{};
    if (value.type().get(type) != simdjson::SUCCESS) {
        return std::unexpected(WireError::WrongType);
    }
    if (type == ondemand::json_type::array) {
        ondemand::array entries;
        if (value.get_array().get(entries) != simdjson::SUCCESS) {
            return std::unexpected(WireError::WrongType);
        }
        for (auto item : entries) {
            ondemand::object fee;
            if (item.get_object().get(fee) != simdjson::SUCCESS) {
                return std::unexpected(WireError::WrongType);
            }
            return wire::opt_str(fee, "totalFee");
        }
        return std::string_view{};
    }
    if (type == ondemand::json_type::object) {
        ondemand::object fee;
        if (value.get_object().get(fee) != simdjson::SUCCESS) {
            return std::unexpected(WireError::WrongType);
        }
        return wire::opt_str(fee, "totalFee");
    }
    return std::string_view{};
}

std::expected<void, WireError> read_order(ondemand::object& obj, WireOrderUpdate& out) {
    // The socket names the instrument instId; both REST lists name it symbol.
    out.inst_id = field(obj, "instId");
    if (out.inst_id.empty()) {
        out.inst_id = field(obj, "symbol");
    }
    if (out.inst_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.order_id = field(obj, "orderId");
    out.client_oid = field(obj, "clientOid");
    out.side = field(obj, "side");
    out.order_type = field(obj, "orderType");
    out.force = field(obj, "force");
    out.price = field(obj, "price");
    if (out.price.empty()) {
        // Spot's pending list carries no `price` at all: while an order is
        // live its limit price arrives under priceAvg.
        out.price = field(obj, "priceAvg");
    }
    out.size = field(obj, "size");
    out.new_size = field(obj, "newSize");
    out.acc_base_volume = field(obj, "accBaseVolume");
    if (out.acc_base_volume.empty()) {
        // The REST lists report the running total under the name the socket
        // reserves for the latest fill alone. Reading one as the other makes
        // a reconciliation pull look like a stream of one-lot fills.
        out.acc_base_volume = field(obj, "baseVolume");
    } else {
        out.base_volume = field(obj, "baseVolume");
    }
    out.fill_price = field(obj, "fillPrice");
    out.fill_fee = field(obj, "fillFee");
    out.trade_id = field(obj, "tradeId");
    out.trade_scope = field(obj, "tradeScope");
    auto status = field(obj, "status");
    if (status.empty()) {
        status = field(obj, "state"); // mix order/detail alone spells it this way
    }
    out.status = order_status_from(status);
    obj.reset();
    out.fill_ms = wire::opt_ms(obj, "fillTime").value_or(0);
    obj.reset();
    out.update_ms = wire::opt_ms(obj, "uTime").value_or(0);
    return {};
}

std::expected<void, WireError> read_fill(ondemand::object& obj, WireFill& out) {
    out.symbol = field(obj, "symbol");
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.order_id = field(obj, "orderId");
    out.trade_id = field(obj, "tradeId");
    out.side = field(obj, "side");
    // Mix reports the trade price as `price`; spot reports the same single
    // trade under `priceAvg`, which on that endpoint averages one fill.
    out.price = field(obj, "price");
    if (out.price.empty()) {
        out.price = field(obj, "priceAvg");
    }
    out.size = field(obj, "baseVolume");
    if (out.size.empty()) {
        out.size = field(obj, "size");
    }
    out.trade_scope = field(obj, "tradeScope");
    const auto fee = read_total_fee(obj);
    if (!fee.has_value()) {
        return std::unexpected(fee.error());
    }
    out.fee = *fee;
    obj.reset();
    out.ts_ms = wire::opt_ms(obj, "cTime").value_or(0);
    return {};
}

std::expected<void, WireError> read_batch_failure(ondemand::object& obj, WireBatchFailure& out) {
    out.order_id = field(obj, "orderId");
    out.client_oid = field(obj, "clientOid");
    out.code = field(obj, "errorCode");
    out.msg = field(obj, "errorMsg");
    return {};
}

} // namespace

WireOrderStatus order_status_from(std::string_view text) noexcept {
    // "new" is not in the documented enum but Bitget's own spot example
    // returns it, and reading it as unknown loses the accept.
    if (text == "live" || text == "new") {
        return WireOrderStatus::Live;
    }
    if (text == "partially_filled") {
        return WireOrderStatus::PartiallyFilled;
    }
    if (text == "filled") {
        return WireOrderStatus::Filled;
    }
    if (text == "cancelled" || text == "canceled") {
        return WireOrderStatus::Canceled;
    }
    return WireOrderStatus::Unknown;
}

std::expected<std::span<const WireOrderUpdate>, WireError>
parse_order_updates(std::string_view frame, OrderUpdateBuffer& storage) {
    return guarded(
        [&] { return read_rows<WireOrderUpdate>(frame, {}, storage.updates, read_order); });
}

std::expected<std::span<const WireOrderUpdate>, WireError>
parse_spot_orders(std::string_view body, OrderUpdateBuffer& storage) {
    return guarded(
        [&] { return read_rows<WireOrderUpdate>(body, {}, storage.updates, read_order); });
}

std::expected<std::span<const WireOrderUpdate>, WireError>
parse_mix_orders(std::string_view body, OrderUpdateBuffer& storage) {
    return guarded([&] {
        return read_rows<WireOrderUpdate>(body, "entrustedList", storage.updates, read_order);
    });
}

std::expected<std::span<const WireFill>, WireError> parse_spot_fills(std::string_view body,
                                                                     FillBuffer& storage) {
    return guarded([&] { return read_rows<WireFill>(body, {}, storage.fills, read_fill); });
}

std::expected<std::span<const WireFill>, WireError> parse_mix_fills(std::string_view body,
                                                                    FillBuffer& storage) {
    return guarded([&] { return read_rows<WireFill>(body, "fillList", storage.fills, read_fill); });
}

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireOrderAck out{};
        const auto order_id = wire::opt_str(*data, "orderId");
        if (!order_id.has_value()) {
            return std::unexpected(order_id.error());
        }
        out.order_id = *order_id;
        const auto client_oid = wire::opt_str(*data, "clientOid");
        if (!client_oid.has_value()) {
            return std::unexpected(client_oid.error());
        }
        out.client_oid = *client_oid;
        if (out.order_id.empty() && out.client_oid.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WireBatchFailure>, WireError>
parse_batch_failures(std::string_view body, BatchFailureBuffer& storage) {
    return guarded([&] {
        return read_rows<WireBatchFailure>(
            body, "failureList", storage.failures, read_batch_failure);
    });
}

} // namespace raikiri::venues::bitget
