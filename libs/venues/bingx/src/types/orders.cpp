#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/orders.hpp>

namespace raikiri::venues::bingx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<WireOrder, WireError> read_order(detail::ondemand::object& obj) {
    WireOrder out{};
    const auto symbol = req_scalar(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected(symbol.error());
    }
    out.symbol = *symbol;
    const auto order_id = opt_scalar(obj, "orderId");
    if (!order_id.has_value()) {
        return std::unexpected(order_id.error());
    }
    out.order_id = *order_id;
    // Three spellings across the two trees and their own worked examples, so
    // the reader takes whichever arrived rather than assuming the endpoint.
    for (const auto key : {"clientOrderId", "clientOrderID", "origClientOrderId"}) {
        const auto value = opt_scalar(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        if (!value->empty()) {
            out.client_order_id = *value;
            break;
        }
    }
    for (const auto& [key, field] : {std::pair{"side", &out.side},
                                     std::pair{"type", &out.type},
                                     std::pair{"price", &out.price},
                                     std::pair{"avgPrice", &out.avg_price},
                                     std::pair{"origQty", &out.orig_qty},
                                     std::pair{"executedQty", &out.executed_qty}}) {
        const auto value = opt_scalar(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    const auto status = opt_scalar(obj, "status");
    if (!status.has_value()) {
        return std::unexpected(status.error());
    }
    out.status = order_status_of(*status);
    const auto updated = opt_ms_any(obj, "updateTime");
    const auto created = opt_ms_any(obj, "time");
    if (!updated.has_value() || !created.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.update_ms = *updated != 0 ? *updated : *created;
    return out;
}

// Case-insensitive because BingX's coin-M push spells the same statuses
// "Pending" and "Filled" where the linear tree shouts them.
bool same_word(std::string_view text, std::string_view word) noexcept {
    if (text.size() != word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char a = text[i] >= 'a' && text[i] <= 'z' ? static_cast<char>(text[i] - 32) : text[i];
        if (a != word[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

WireOrderStatus order_status_of(std::string_view text) noexcept {
    if (same_word(text, "NEW") || same_word(text, "PENDING")) {
        return WireOrderStatus::New;
    }
    if (same_word(text, "PARTIALLY_FILLED")) {
        return WireOrderStatus::PartiallyFilled;
    }
    if (same_word(text, "FILLED")) {
        return WireOrderStatus::Filled;
    }
    // Both spellings are BingX's own: the cancel answer says CANCELLED and the
    // documented status list says CANCELED.
    if (same_word(text, "CANCELED") || same_word(text, "CANCELLED")) {
        return WireOrderStatus::Canceled;
    }
    if (same_word(text, "EXPIRED")) {
        return WireOrderStatus::Expired;
    }
    if (same_word(text, "FAILED")) {
        return WireOrderStatus::Rejected;
    }
    return WireOrderStatus::Unknown;
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                  OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto array = detail::req_array(*data, "orders");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_orders) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto order = read_order(obj);
            if (!order.has_value()) {
                return std::unexpected(order.error());
            }
            storage.orders[count] = *order;
            ++count;
        }
        return std::span<const WireOrder>{storage.orders.data(), count};
    });
}

std::expected<WireOrder, WireError> parse_order_ack(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        // The perpetual tree nests the order one level deeper than spot does.
        detail::ondemand::object nested;
        if (data->find_field_unordered("order").get_object().get(nested) == simdjson::SUCCESS) {
            return read_order(nested);
        }
        return read_order(*data);
    });
}

std::expected<WireOrderUpdate, WireError> parse_order_update(std::string_view frame) {
    return guarded([&]() -> std::expected<WireOrderUpdate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // The perpetual push puts the order under `o` and the spot push wraps
        // the whole record in `data`.
        detail::ondemand::object obj;
        if (root->find_field_unordered("o").get_object().get(obj) != simdjson::SUCCESS) {
            auto data = detail::req_object(*root, "data");
            if (!data.has_value()) {
                return std::unexpected(data.error());
            }
            obj = *data;
        }

        WireOrderUpdate out{};
        const auto symbol = req_scalar(obj, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        for (const auto& [key, field] : {std::pair{"c", &out.client_order_id},
                                         std::pair{"i", &out.order_id},
                                         std::pair{"S", &out.side},
                                         std::pair{"o", &out.type},
                                         std::pair{"x", &out.exec_type},
                                         std::pair{"p", &out.price},
                                         std::pair{"q", &out.orig_qty},
                                         std::pair{"l", &out.last_qty},
                                         std::pair{"z", &out.cum_qty},
                                         std::pair{"L", &out.last_price},
                                         std::pair{"n", &out.fee},
                                         std::pair{"t", &out.trade_id}}) {
            const auto value = opt_scalar(obj, key);
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            *field = *value;
        }
        const auto status = opt_scalar(obj, "X");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        out.status = order_status_of(*status);
        const auto maker = opt_scalar(obj, "m");
        if (!maker.has_value()) {
            return std::unexpected(maker.error());
        }
        out.maker_known = !maker->empty();
        out.is_maker = *maker == "true";
        const auto ts = opt_ms_any(obj, "T");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        out.ts_ms = *ts;
        return out;
    });
}

} // namespace raikiri::venues::bingx
