#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer; a
// venue that reimplements opt_str or req_array is one that will disagree with
// its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

// A field WEEX quotes as a string in one answer and as an 18-digit number in
// the next, read off the RAW TOKEN so both arrive as the same text. Reading it
// as a float loses the low digits — the venue's own documentation prints an
// order id three digits short for exactly that reason — and reading it as a
// string fails outright on the reads that send a number.
[[nodiscard]] std::string_view id_text(ondemand::object& obj, std::string_view key) {
    obj.reset();
    auto raw = detail::opt_raw(obj, key).value_or(std::string_view{});
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    return raw;
}

std::expected<void, WireError> read_order(ondemand::object& obj, WireOrder& out) {
    auto symbol = detail::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;

    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    out.client_order_id = field("clientOrderId");
    out.side = field("side");
    out.price = field("price");
    out.orig_qty = field("origQty");
    out.executed_qty = field("executedQty");
    out.status = order_status_from(field("status"));
    out.order_id = id_text(obj, "orderId");
    obj.reset();
    // `updateTime` is the one that moves as an order fills; `time` never
    // changes after the order was created.
    out.update_ms = detail::opt_u64(obj, "updateTime").value_or(0);
    return {};
}

std::expected<void, WireError> read_fill(ondemand::object& obj, WireFill& out) {
    auto symbol = detail::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;

    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    out.side = field("side");
    out.price = field("price");
    out.qty = field("qty");
    out.commission = field("commission");
    out.fill_id = id_text(obj, "id");
    out.order_id = id_text(obj, "orderId");
    obj.reset();
    out.maker = detail::opt_bool(obj, "maker").value_or(false);
    obj.reset();
    out.fill_ms = detail::opt_u64(obj, "time").value_or(0);
    return {};
}

} // namespace

WireOrderStatus order_status_from(std::string_view text) noexcept {
    if (text == "NEW") {
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

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                  OrderBuffer& storage) {
    return guarded([&] { return read_array<WireOrder>(body, storage.orders, read_order); });
}

namespace {

constexpr std::string_view k_error_order_not_found = "ORDER_NOT_FOUND";

std::expected<void, WireError> read_ack(ondemand::object& obj, WireOrderAck& out) {
    out.order_id = id_text(obj, "orderId");
    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    // The place answer echoes `clientOrderId` and the cancel answer
    // `origClientOrderId`; one ack type reads both rather than two types
    // differing by a field name.
    out.client_order_id = field("clientOrderId");
    if (out.client_order_id.empty()) {
        out.client_order_id = field("origClientOrderId");
    }
    out.error_code = field("errorCode");
    out.error_message = field("errorMessage");
    obj.reset();
    const auto accepted = detail::opt_bool(obj, "success");
    if (!accepted.has_value()) {
        return std::unexpected{accepted.error()};
    }
    // ABSENT READS AS REFUSED. opt_bool answers false for a missing key, and an
    // ack whose verdict this process could not find must not be taken for an
    // acceptance.
    out.success = *accepted;
    return {};
}

} // namespace

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto root = detail::session().open_object(body);
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

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                FillBuffer& storage) {
    return guarded([&] { return read_array<WireFill>(body, storage.fills, read_fill); });
}

} // namespace raikiri::venues::weex
