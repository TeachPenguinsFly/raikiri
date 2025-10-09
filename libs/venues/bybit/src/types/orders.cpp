#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

// Reads one order-shaped object. Shared because /v5/order/realtime returns
// the same shape as the websocket `order` topic — the venue is consistent
// here, so the parser is too.
std::expected<void, WireError> read_order(ondemand::object& obj, WireOrderUpdate& out) {
    auto symbol = detail::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;

    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    out.order_id = field("orderId");
    out.order_link_id = field("orderLinkId"); // our ULID returns here
    out.side = field("side");
    out.order_type = field("orderType");
    out.price = field("price");
    out.qty = field("qty");
    out.leaves_qty = field("leavesQty");
    out.cum_exec_qty = field("cumExecQty");
    out.avg_price = field("avgPrice");
    out.reject_reason = field("rejectReason");
    out.status = order_status_from(field("orderStatus"));
    // Timestamps are milliseconds inside a JSON string on this stream.
    out.updated_ms = detail::digits_to_u64(field("updatedTime"));
    return {};
}

} // namespace

WireOrderStatus order_status_from(std::string_view text) noexcept {
    if (text == "New") {
        return WireOrderStatus::New;
    }
    if (text == "PartiallyFilled") {
        return WireOrderStatus::PartiallyFilled;
    }
    if (text == "Untriggered") {
        return WireOrderStatus::Untriggered;
    }
    if (text == "Triggered") {
        return WireOrderStatus::Triggered;
    }
    if (text == "Deactivated") {
        return WireOrderStatus::Deactivated;
    }
    if (text == "Rejected") {
        return WireOrderStatus::Rejected;
    }
    if (text == "Cancelled") {
        return WireOrderStatus::Cancelled;
    }
    if (text == "PartiallyFilledCanceled") {
        return WireOrderStatus::PartiallyFilledCanceled;
    }
    if (text == "Filled") {
        return WireOrderStatus::Filled;
    }
    return WireOrderStatus::Unknown;
}

std::expected<std::span<const WireOrderUpdate>, WireError>
parse_order_updates(std::string_view frame, OrderUpdateBuffer& storage) {
    return guarded([&] {
        return read_list<WireOrderUpdate>(
            frame,
            "data",
            storage.updates,
            k_max_updates,
            [](ondemand::object& obj, WireOrderUpdate& out) { return read_order(obj, out); });
    });
}

std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view frame, ExecutionBuffer& storage) {
    return guarded([&] {
        return read_list<WireExecution>(
            frame,
            "data",
            storage.executions,
            k_max_updates,
            [](ondemand::object& obj, WireExecution& out) -> std::expected<void, WireError> {
                auto symbol = detail::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected{symbol.error()};
                }
                out.symbol = *symbol;
                const auto field = [&](std::string_view key) {
                    obj.reset();
                    return detail::opt_str(obj, key).value_or(std::string_view{});
                };
                out.exec_type = field("execType");
                out.exec_id = field("execId");
                out.order_id = field("orderId");
                out.order_link_id = field("orderLinkId");
                out.side = field("side");
                out.exec_price = field("execPrice");
                out.exec_qty = field("execQty");
                out.exec_fee = field("execFee");
                out.leaves_qty = field("leavesQty");
                out.exec_ms = detail::digits_to_u64(field("execTime"));
                obj.reset();
                out.is_maker = detail::opt_bool(obj, "isMaker").value_or(false);
                return {};
            });
    });
}

std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view result, OrderUpdateBuffer& storage) {
    return guarded([&] {
        return read_list<WireOpenOrder>(
            result,
            "list",
            storage.updates,
            k_max_updates,
            [](ondemand::object& obj, WireOpenOrder& out) { return read_order(obj, out); });
    });
}

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view result) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(result);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        WireOrderAck out{};
        out.order_id = detail::opt_str(obj, "orderId").value_or(std::string_view{});
        obj.reset();
        out.order_link_id = detail::opt_str(obj, "orderLinkId").value_or(std::string_view{});
        if (out.order_id.empty() && out.order_link_id.empty()) {
            return std::unexpected{WireError::MissingField};
        }
        return out;
    });
}

} // namespace raikiri::venues::bybit
