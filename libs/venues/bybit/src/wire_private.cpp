// Private-stream frames: order updates, executions, positions.

#include "wire_detail.hpp"

namespace raikiri::venues::bybit {

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

template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError> read_array(std::string_view frame,
                                                            std::string_view key, Entry* storage,
                                                            std::size_t capacity, Read&& read) {
    auto& session = detail::session();
    auto root = session.open_object(frame);
    if (!root.has_value()) {
        return std::unexpected{root.error()};
    }
    auto obj = *root;

    auto data = detail::req_array(obj, key);
    if (!data.has_value()) {
        return std::unexpected{data.error()};
    }
    std::size_t count = 0;
    for (auto item : *data) {
        if (count == capacity) {
            return std::unexpected{WireError::TooManyEntries};
        }
        ondemand::object entry_obj;
        if (item.get_object().get(entry_obj) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        Entry entry{};
        if (auto ok = read(entry_obj, entry); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        storage[count++] = entry;
    }
    return std::span<const Entry>{storage, count};
}

} // namespace

WireOrderStatus order_status_from(std::string_view text) noexcept {
    // PartiallyFilledCanceled is the trap: it reads like a partial fill but
    // is terminal — the remainder was cancelled. Mapping it to
    // PartiallyFilled would leave the order open forever in our store.
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
        return read_array<WireOrderUpdate>(frame, "data", storage.updates, k_max_updates,
                                           [](ondemand::object& obj, WireOrderUpdate& out) {
                                               return read_order(obj, out);
                                           });
    });
}

std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view frame, ExecutionBuffer& storage) {
    return guarded([&] {
        return read_array<WireExecution>(
            frame, "data", storage.executions, k_max_updates,
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
                // Liquidity flag decides the fee sign on most venues and the
                // maker/taker split in our PnL, so it is read explicitly
                // rather than inferred from the fee.
                out.is_maker = detail::opt_bool(obj, "isMaker").value_or(false);
                return {};
            });
    });
}

std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view frame, PositionBuffer& storage) {
    return guarded([&] {
        return read_array<WirePosition>(
            frame, "data", storage.positions, k_max_updates,
            [](ondemand::object& obj, WirePosition& out) -> std::expected<void, WireError> {
                auto symbol = detail::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected{symbol.error()};
                }
                out.symbol = *symbol;
                const auto field = [&](std::string_view key) {
                    obj.reset();
                    return detail::opt_str(obj, key).value_or(std::string_view{});
                };
                // An empty side with size "0" is bybit's way of saying flat.
                out.side = field("side");
                out.size = field("size");
                out.avg_price = field("entryPrice");
                out.updated_ms = detail::digits_to_u64(field("updatedTime"));
                return {};
            });
    });
}

std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view result, OrderUpdateBuffer& storage) {
    return guarded([&] {
        return read_array<WireOpenOrder>(result, "list", storage.updates, k_max_updates,
                                         [](ondemand::object& obj, WireOpenOrder& out) {
                                             return read_order(obj, out);
                                         });
    });
}

} // namespace raikiri::venues::bybit
