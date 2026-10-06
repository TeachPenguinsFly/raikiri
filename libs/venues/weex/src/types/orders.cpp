#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
namespace ondemand = wire::ondemand;

// ids arrive as a string or an 18-digit number; read the raw token
[[nodiscard]] std::string_view id_text(ondemand::object& obj, std::string_view key) {
    obj.reset();
    auto raw = wire::opt_raw(obj, key).value_or(std::string_view{});
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    return raw;
}

std::expected<void, WireError> read_order(ondemand::object& obj, WireOrder& out) {
    auto symbol = wire::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;

    const auto field = [&](std::string_view key) {
        obj.reset();
        return wire::opt_str(obj, key).value_or(std::string_view{});
    };
    out.client_order_id = field("clientOrderId");
    out.side = field("side");
    out.price = field("price");
    out.orig_qty = field("origQty");
    out.executed_qty = field("executedQty");
    out.status = order_status_from(field("status"));
    out.order_id = id_text(obj, "orderId");
    obj.reset();
    // updateTime moves as the order fills; time never changes
    out.update_ms = wire::opt_u64(obj, "updateTime").value_or(0);
    return {};
}

std::expected<void, WireError> read_fill(ondemand::object& obj, WireFill& out) {
    auto symbol = wire::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;

    const auto field = [&](std::string_view key) {
        obj.reset();
        return wire::opt_str(obj, key).value_or(std::string_view{});
    };
    out.side = field("side");
    out.price = field("price");
    out.qty = field("qty");
    out.commission = field("commission");
    out.fill_id = id_text(obj, "id");
    out.order_id = id_text(obj, "orderId");
    obj.reset();
    out.maker = wire::opt_bool(obj, "maker").value_or(false);
    obj.reset();
    out.fill_ms = wire::opt_u64(obj, "time").value_or(0);
    return {};
}

} // namespace

WireOrderStatus order_status_from(std::string_view text) noexcept {
    if (text == "NEW") {
        return WireOrderStatus::New;
    }
    if (text == "PARTIALLY_FILLED") {
        return WireOrderStatus::New;
    }
    if (text == "PENDING") {
        return WireOrderStatus::Pending;
    }
    if (text == "UNTRIGGERED") {
        return WireOrderStatus::Untriggered;
    }
    if (text == "UNACTIVATED") {
        return WireOrderStatus::Unactivated;
    }
    if (text == "FILLED") {
        return WireOrderStatus::Filled;
    }
    if (text == "CANCELED") {
        return WireOrderStatus::Canceled;
    }
    if (text == "CANCELING") {
        return WireOrderStatus::Canceling;
    }
    return WireOrderStatus::Unknown;
}

std::expected<WireOrder, WireError> parse_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        WireOrder out{};
        if (auto ok = read_order(obj, out); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        return out;
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body, OrderBuffer& storage) {
    return guarded([&] { return read_array<WireOrder>(body, storage.orders, read_order); });
}

namespace {

constexpr std::string_view k_error_order_not_found = "ORDER_NOT_FOUND";

std::expected<void, WireError> read_ack(ondemand::object& obj, WireOrderAck& out) {
    out.order_id = id_text(obj, "orderId");
    const auto field = [&](std::string_view key) {
        obj.reset();
        return wire::opt_str(obj, key).value_or(std::string_view{});
    };
    // place echoes clientOrderId, cancel origClientOrderId
    out.client_order_id = field("clientOrderId");
    if (out.client_order_id.empty()) {
        out.client_order_id = field("origClientOrderId");
    }
    out.error_code = field("errorCode");
    out.error_message = field("errorMessage");
    obj.reset();
    const auto accepted = wire::opt_bool(obj, "success");
    if (!accepted.has_value()) {
        return std::unexpected{accepted.error()};
    }
    // absent reads as refused
    out.success = *accepted;
    return {};
}

} // namespace

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        WireOrderAck out{};
        if (auto ok = read_ack(obj, out); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        return out;
    });
}

std::expected<std::span<const WireOrderAck>, WireError> parse_order_acks(std::string_view body,
                                                                         OrderAckBuffer& storage) {
    return guarded([&] { return read_array<WireOrderAck>(body, storage.acks, read_ack); });
}

bool is_benign_cancel_error(std::string_view error_code) noexcept {
    return error_code == k_error_order_not_found;
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body, FillBuffer& storage) {
    return guarded([&] { return read_array<WireFill>(body, storage.fills, read_fill); });
}

} // namespace raikiri::venues::weex
