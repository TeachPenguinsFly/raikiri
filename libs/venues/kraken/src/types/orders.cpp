#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kraken {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::string_view status_key(StatusKind kind) noexcept {
    switch (kind) {
    case StatusKind::Send:
        return "sendStatus";
    case StatusKind::Cancel:
        return "cancelStatus";
    case StatusKind::Edit:
        return "editStatus";
    }
    return "sendStatus";
}

// An ISO-8601 instant that is absent or unreadable yields 0 rather than a
// failure: a verdict is still a verdict without its timestamp, and the caller
// substitutes ingest time.
std::uint64_t iso_ms(std::string_view text) noexcept {
    if (text.empty()) {
        return 0;
    }
    const auto ms = parse_iso8601_ms(text);
    return ms.has_value() ? *ms : 0;
}

// sendorder answers `order_id`; editorder answers `orderId`. One venue, one
// verdict object, two spellings — reading only the documented one loses the
// venue id on every amend.
std::expected<std::string_view, WireError> order_id_of(detail::ondemand::object& obj) {
    const auto snake = detail::opt_str(obj, "order_id");
    if (!snake.has_value()) {
        return std::unexpected(snake.error());
    }
    if (!snake->empty()) {
        return *snake;
    }
    return detail::opt_str(obj, "orderId");
}

std::expected<WireOrderStatus, WireError> read_status(detail::ondemand::object& obj) {
    WireOrderStatus out{};
    const auto status = detail::req_str(obj, "status");
    const auto order_id = order_id_of(obj);
    const auto cli_ord_id = detail::opt_str(obj, "cliOrdId");
    if (!status.has_value() || !order_id.has_value() || !cli_ord_id.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.status = *status;
    out.order_id = *order_id;
    out.cli_ord_id = *cli_ord_id;
    const auto received = detail::opt_str(obj, "receivedTime");
    if (!received.has_value()) {
        return std::unexpected(received.error());
    }
    out.received_ms = iso_ms(*received);
    return out;
}

std::expected<WireWsOrder, WireError> read_ws_order(detail::ondemand::object& obj) {
    WireWsOrder out{};
    const auto instrument = detail::req_str(obj, "instrument");
    const auto order_id = detail::req_str(obj, "order_id");
    const auto cli_ord_id = detail::opt_str(obj, "cli_ord_id");
    const auto type = detail::opt_str(obj, "type");
    if (!instrument.has_value() || !order_id.has_value() || !cli_ord_id.has_value() ||
        !type.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.instrument = *instrument;
    out.order_id = *order_id;
    out.cli_ord_id = *cli_ord_id;
    out.type = *type;
    const auto qty = req_num(obj, "qty");
    const auto filled = opt_num(obj, "filled");
    const auto limit_price = opt_num(obj, "limit_price");
    const auto stop_price = opt_num(obj, "stop_price");
    if (!qty.has_value() || !filled.has_value() || !limit_price.has_value() ||
        !stop_price.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.remaining_qty = *qty;
    out.filled_qty = *filled;
    out.limit_price = *limit_price;
    out.stop_price = *stop_price;
    const auto time_ms = opt_ms(obj, "time");
    const auto last_update = opt_ms(obj, "last_update_time");
    if (!time_ms.has_value() || !last_update.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.time_ms = *time_ms;
    out.last_update_ms = *last_update;
    // `direction` is 0 for buy and 1 for sell; there is no side string here,
    // so an absent field is not a default — it is a frame whose side we do not
    // know, and guessing one would put the position on the wrong side.
    bool has_direction = false;
    const auto direction = opt_i64(obj, "direction", has_direction);
    const auto reduce_only = detail::opt_bool(obj, "reduce_only");
    if (!direction.has_value() || !has_direction || !reduce_only.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.buy = *direction == 0;
    out.reduce_only = *reduce_only;
    return out;
}

std::expected<WireWsFill, WireError> read_ws_fill(detail::ondemand::object& obj) {
    WireWsFill out{};
    const auto instrument = detail::req_str(obj, "instrument");
    const auto order_id = detail::req_str(obj, "order_id");
    const auto fill_id = detail::req_str(obj, "fill_id");
    const auto cli_ord_id = detail::opt_str(obj, "cli_ord_id");
    const auto fill_type = detail::opt_str(obj, "fill_type");
    const auto fee_currency = detail::opt_str(obj, "fee_currency");
    if (!instrument.has_value() || !order_id.has_value() || !fill_id.has_value() ||
        !cli_ord_id.has_value() || !fill_type.has_value() || !fee_currency.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.instrument = *instrument;
    out.order_id = *order_id;
    out.fill_id = *fill_id;
    out.cli_ord_id = *cli_ord_id;
    out.fill_type = *fill_type;
    out.fee_currency = *fee_currency;
    const auto price = req_num(obj, "price");
    const auto qty = req_num(obj, "qty");
    const auto remaining = opt_num(obj, "remaining_order_qty");
    const auto fee_paid = opt_num(obj, "fee_paid");
    if (!price.has_value() || !qty.has_value() || !remaining.has_value() || !fee_paid.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.price = *price;
    out.qty = *qty;
    out.remaining_order_qty = *remaining;
    out.fee_paid = *fee_paid;
    const auto time_ms = opt_ms(obj, "time");
    const auto buy = detail::opt_bool(obj, "buy");
    if (!time_ms.has_value() || !buy.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.time_ms = *time_ms;
    out.buy = *buy;
    return out;
}

} // namespace

std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body,
                                                             StatusKind kind) {
    return guarded([&]() -> std::expected<WireOrderStatus, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = detail::req_object(*root, status_key(kind));
        if (!obj.has_value()) {
            return std::unexpected(obj.error());
        }
        return read_status(*obj);
    });
}

std::expected<std::span<const WireOrderStatus>, WireError>
parse_batch_status(std::string_view body, BatchStatusBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrderStatus>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "batchStatus");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_batch) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto row = read_status(obj);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            storage.statuses[count] = *row;
            ++count;
        }
        return std::span<const WireOrderStatus>{storage.statuses.data(), count};
    });
}

std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view body, OpenOrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOpenOrder>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "openOrders");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_open_orders) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireOpenOrder out{};
            const auto order_id = detail::req_str(obj, "order_id");
            const auto cli_ord_id = detail::opt_str(obj, "cliOrdId");
            const auto symbol = detail::req_str(obj, "symbol");
            const auto status = detail::opt_str(obj, "status");
            const auto order_type = detail::opt_str(obj, "orderType");
            const auto side = detail::req_str(obj, "side");
            if (!order_id.has_value() || !cli_ord_id.has_value() || !symbol.has_value() ||
                !status.has_value() || !order_type.has_value() || !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.order_id = *order_id;
            out.cli_ord_id = *cli_ord_id;
            out.symbol = *symbol;
            out.status = *status;
            out.order_type = *order_type;
            out.buy = *side == "buy";
            const auto limit_price = opt_num(obj, "limitPrice");
            const auto filled = opt_num(obj, "filledSize");
            const auto unfilled = opt_num(obj, "unfilledSize");
            if (!limit_price.has_value() || !filled.has_value() || !unfilled.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.limit_price = *limit_price;
            out.filled_size = *filled;
            out.unfilled_size = *unfilled;
            const auto last_update = detail::opt_str(obj, "lastUpdateTime");
            const auto reduce_only = detail::opt_bool(obj, "reduceOnly");
            if (!last_update.has_value() || !reduce_only.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.last_update_ms = iso_ms(*last_update);
            out.reduce_only = *reduce_only;
            storage.orders[count] = out;
            ++count;
        }
        return std::span<const WireOpenOrder>{storage.orders.data(), count};
    });
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                FillBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFill>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "fills");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_fills) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireFill out{};
            const auto fill_id = detail::req_str(obj, "fill_id");
            const auto order_id = detail::req_str(obj, "order_id");
            const auto cli_ord_id = detail::opt_str(obj, "cliOrdId");
            const auto symbol = detail::req_str(obj, "symbol");
            const auto side = detail::req_str(obj, "side");
            const auto fill_type = detail::opt_str(obj, "fillType");
            const auto fill_time = detail::opt_str(obj, "fillTime");
            if (!fill_id.has_value() || !order_id.has_value() || !cli_ord_id.has_value() ||
                !symbol.has_value() || !side.has_value() || !fill_type.has_value() ||
                !fill_time.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.fill_id = *fill_id;
            out.order_id = *order_id;
            out.cli_ord_id = *cli_ord_id;
            out.symbol = *symbol;
            out.buy = *side == "buy";
            out.fill_type = *fill_type;
            out.fill_time = *fill_time;
            out.fill_ms = iso_ms(*fill_time);
            const auto price = req_num(obj, "price");
            const auto size = req_num(obj, "size");
            if (!price.has_value() || !size.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price = *price;
            out.size = *size;
            storage.fills[count] = out;
            ++count;
        }
        return std::span<const WireFill>{storage.fills.data(), count};
    });
}

std::expected<WireOrderDelta, WireError> parse_order_delta(std::string_view frame) {
    return guarded([&]() -> std::expected<WireOrderDelta, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireOrderDelta out{};
        const auto reason = detail::opt_str(*root, "reason");
        const auto is_cancel = detail::opt_bool(*root, "is_cancel");
        if (!reason.has_value() || !is_cancel.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.reason = *reason;
        out.is_cancel = *is_cancel;
        if (detail::has_field(*root, "order")) {
            auto obj = detail::req_object(*root, "order");
            if (!obj.has_value()) {
                return std::unexpected(obj.error());
            }
            const auto order = read_ws_order(*obj);
            if (!order.has_value()) {
                return std::unexpected(order.error());
            }
            out.order = *order;
            out.has_order = true;
            out.order_id = out.order.order_id;
            return out;
        }
        const auto order_id = detail::req_str(*root, "order_id");
        if (!order_id.has_value()) {
            return std::unexpected(order_id.error());
        }
        out.order_id = *order_id;
        return out;
    });
}

std::expected<std::span<const WireWsOrder>, WireError>
parse_order_snapshot(std::string_view frame, OrderSnapshotBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireWsOrder>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "orders");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_open_orders) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto order = read_ws_order(obj);
            if (!order.has_value()) {
                return std::unexpected(order.error());
            }
            storage.orders[count] = *order;
            ++count;
        }
        return std::span<const WireWsOrder>{storage.orders.data(), count};
    });
}

std::expected<std::span<const WireWsFill>, WireError> parse_ws_fills(std::string_view frame,
                                                                     WsFillBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireWsFill>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "fills");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_fills) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto fill = read_ws_fill(obj);
            if (!fill.has_value()) {
                return std::unexpected(fill.error());
            }
            storage.fills[count] = *fill;
            ++count;
        }
        return std::span<const WireWsFill>{storage.fills.data(), count};
    });
}

} // namespace raikiri::venues::kraken
