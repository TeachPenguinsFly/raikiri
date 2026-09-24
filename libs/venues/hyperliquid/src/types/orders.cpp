#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

namespace {

using wire::guarded;
using wire::session;

// "B" bids, "A" asks
[[nodiscard]] bool is_buy(std::string_view side) noexcept {
    return side == "B";
}

// absent or null reads as empty
[[nodiscard]] std::expected<std::string_view, WireError> soft_str(wire::ondemand::object& obj,
                                                                  std::string_view key) {
    const auto value = wire::opt_str(obj, key);
    if (!value.has_value() && value.error() == WireError::MissingField) {
        return std::string_view{};
    }
    return value;
}

[[nodiscard]] std::expected<bool, WireError> soft_bool(wire::ondemand::object& obj,
                                                       std::string_view key) {
    const auto value = wire::opt_bool(obj, key);
    if (!value.has_value() && value.error() == WireError::MissingField) {
        return false;
    }
    return value;
}

std::expected<WireOpenOrder, WireError> read_open_order(wire::ondemand::object& obj) {
    WireOpenOrder out{};
    const auto coin = wire::req_str(obj, "coin");
    const auto side = wire::req_str(obj, "side");
    const auto price = wire::req_str(obj, "limitPx");
    const auto qty = wire::req_str(obj, "sz");
    const auto orig = soft_str(obj, "origSz");
    const auto cloid = soft_str(obj, "cloid");
    const auto order_type = soft_str(obj, "orderType");
    const auto tif = soft_str(obj, "tif");
    const auto oid = wire::opt_u64(obj, "oid");
    const auto ts = opt_number_ms(obj, "timestamp");
    const auto reduce_only = soft_bool(obj, "reduceOnly");
    const auto is_trigger = soft_bool(obj, "isTrigger");
    if (!coin.has_value() || !side.has_value() || !price.has_value() || !qty.has_value() ||
        !orig.has_value() || !cloid.has_value() || !order_type.has_value() || !tif.has_value() ||
        !oid.has_value() || !ts.has_value() || !reduce_only.has_value() ||
        !is_trigger.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.coin = *coin;
    out.buy = is_buy(*side);
    out.price = *price;
    out.qty = *qty;
    out.orig_qty = *orig;
    out.cloid = *cloid;
    out.order_type = *order_type;
    out.tif = *tif;
    out.oid = *oid;
    out.ts_ms = *ts;
    out.reduce_only = *reduce_only;
    out.is_trigger = *is_trigger;
    return out;
}

[[nodiscard]] ExchangeOutcome outcome_from_word(std::string_view word) noexcept {
    if (word == "success") {
        return ExchangeOutcome::Success;
    }
    // tp/sl orders answer this bare string
    if (word == "waitingForTrigger") {
        return ExchangeOutcome::WaitingForTrigger;
    }
    return ExchangeOutcome::Unknown;
}

std::expected<WireExchangeStatus, WireError> read_status(wire::ondemand::value entry) {
    wire::ondemand::json_type kind{};
    if (const auto ec = entry.type().get(kind); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    WireExchangeStatus out{};
    if (kind == wire::ondemand::json_type::string) {
        const auto word = wire::str_of(entry);
        if (!word.has_value()) {
            return std::unexpected(word.error());
        }
        out.outcome = outcome_from_word(*word);
        return out;
    }
    wire::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    // the single key is the outcome; on-demand cannot probe names, so walk the fields
    for (auto field : obj) {
        std::string_view key;
        if (const auto ec = field.unescaped_key().get(key); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (key == "error") {
            const auto text = wire::str_of(field.value());
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            out.outcome = ExchangeOutcome::Error;
            out.error = *text;
            return out;
        }
        wire::ondemand::object inner;
        if (const auto ec = field.value().get_object().get(inner); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (key == "resting") {
            const auto oid = wire::opt_u64(inner, "oid");
            if (!oid.has_value()) {
                return std::unexpected(oid.error());
            }
            out.outcome = ExchangeOutcome::Resting;
            out.oid = *oid;
            return out;
        }
        if (key == "filled") {
            const auto qty = wire::req_str(inner, "totalSz");
            const auto price = wire::req_str(inner, "avgPx");
            const auto oid = wire::opt_u64(inner, "oid");
            if (!qty.has_value() || !price.has_value() || !oid.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.outcome = ExchangeOutcome::Filled;
            out.total_qty = *qty;
            out.avg_price = *price;
            out.oid = *oid;
            return out;
        }
        return out; // an outcome word this adapter does not know
    }
    return out;
}

} // namespace

std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view body, OpenOrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOpenOrder>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto row : *root) {
            if (count == k_max_open_orders) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto order = read_open_order(obj);
            if (!order.has_value()) {
                return std::unexpected(order.error());
            }
            storage.orders[count] = *order;
            ++count;
        }
        return std::span<const WireOpenOrder>{storage.orders.data(), count};
    });
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                FillBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFill>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto row : *root) {
            if (count == k_max_fills) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireFill out{};
            const auto coin = wire::req_str(obj, "coin");
            const auto price = wire::req_str(obj, "px");
            const auto qty = wire::req_str(obj, "sz");
            const auto side = wire::req_str(obj, "side");
            const auto fee = wire::req_str(obj, "fee");
            const auto fee_token = soft_str(obj, "feeToken");
            const auto cloid = soft_str(obj, "cloid");
            const auto oid = wire::opt_u64(obj, "oid");
            const auto tid = wire::opt_u64(obj, "tid");
            const auto ts = opt_number_ms(obj, "time");
            const auto crossed = soft_bool(obj, "crossed");
            if (!coin.has_value() || !price.has_value() || !qty.has_value() || !side.has_value() ||
                !fee.has_value() || !fee_token.has_value() || !cloid.has_value() ||
                !oid.has_value() || !tid.has_value() || !ts.has_value() || !crossed.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.coin = *coin;
            out.price = *price;
            out.qty = *qty;
            out.fee = *fee;
            out.fee_token = *fee_token;
            out.cloid = *cloid;
            out.oid = *oid;
            out.tid = *tid;
            out.ts_ms = *ts;
            out.buy = is_buy(*side);
            out.crossed = *crossed;
            storage.fills[count] = out;
            ++count;
        }
        return std::span<const WireFill>{storage.fills.data(), count};
    });
}

std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrderStatus, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto outer = wire::req_str(*root, "status");
        if (!outer.has_value()) {
            return std::unexpected(outer.error());
        }
        WireOrderStatus out{};
        if (*outer != "order") {
            return out; // "unknownOid": not found, and not an error either
        }
        auto wrapper = wire::req_object(*root, "order");
        if (!wrapper.has_value()) {
            return std::unexpected(wrapper.error());
        }
        auto inner = wire::req_object(*wrapper, "order");
        if (!inner.has_value()) {
            return std::unexpected(inner.error());
        }
        const auto order = read_open_order(*inner);
        if (!order.has_value()) {
            return std::unexpected(order.error());
        }
        // read after the nested object; on-demand cannot revisit fields
        const auto status = wire::req_str(*wrapper, "status");
        const auto ts = opt_number_ms(*wrapper, "statusTimestamp");
        if (!status.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.order = *order;
        out.status = *status;
        out.ts_ms = *ts;
        out.found = true;
        return out;
    });
}

std::expected<WireExchangeReply, WireError> parse_exchange_reply(std::string_view body,
                                                                 ExchangeStatusBuffer& storage) {
    return guarded([&]() -> std::expected<WireExchangeReply, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = wire::req_str(*root, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        WireExchangeReply out{};
        out.ok = *status == "ok";

        wire::ondemand::value response;
        if (const auto ec = wire::find(*root, "response").get(response); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        wire::ondemand::json_type kind{};
        if (const auto ec = response.type().get(kind); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (kind == wire::ondemand::json_type::string) {
            // a top-level refusal makes `response` a string
            const auto text = wire::str_of(response);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            out.message = *text;
            return out;
        }

        wire::ondemand::object envelope;
        if (const auto ec = response.get_object().get(envelope); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto type = wire::req_str(envelope, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        out.type = *type;

        auto data = wire::req_object(envelope, "data");
        if (!data.has_value()) {
            // several actions answer {"type":"default"} alone
            return data.error() == WireError::MissingField
                       ? std::expected<WireExchangeReply, WireError>{out}
                       : std::unexpected(data.error());
        }
        auto rows = wire::req_array(*data, "statuses");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto row : *rows) {
            if (count == k_max_statuses) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::value entry;
            if (const auto ec = row.get(entry); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto parsed = read_status(entry);
            if (!parsed.has_value()) {
                return std::unexpected(parsed.error());
            }
            storage.statuses[count] = *parsed;
            ++count;
        }
        out.statuses = std::span<const WireExchangeStatus>{storage.statuses.data(), count};
        return out;
    });
}

} // namespace raikiri::venues::hyperliquid
