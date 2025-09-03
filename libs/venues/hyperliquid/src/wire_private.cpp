#include <array>

#include "wire_detail.hpp"

namespace raikiri::venues::hyperliquid {

namespace {

using detail::guarded;
using detail::session;

// "B" bids, "A" asks — the venue's spelling everywhere a side appears, on both
// the account reads and the public trade prints.
[[nodiscard]] bool is_buy(std::string_view side) noexcept {
    return side == "B";
}

// An optional STRING field: absent reads as empty, which is what `cloid` does
// on an order placed without one. A present JSON null reads as empty too.
[[nodiscard]] std::expected<std::string_view, WireError> soft_str(detail::ondemand::object& obj,
                                                                  std::string_view key) {
    const auto value = detail::opt_str(obj, key);
    if (!value.has_value() && value.error() == WireError::MissingField) {
        return std::string_view{};
    }
    return value;
}

[[nodiscard]] std::expected<bool, WireError> soft_bool(detail::ondemand::object& obj,
                                                       std::string_view key) {
    const auto value = detail::opt_bool(obj, key);
    if (!value.has_value() && value.error() == WireError::MissingField) {
        return false;
    }
    return value;
}

std::expected<WireOpenOrder, WireError> read_open_order(detail::ondemand::object& obj) {
    WireOpenOrder out{};
    const auto coin = detail::req_str(obj, "coin");
    const auto side = detail::req_str(obj, "side");
    const auto price = detail::req_str(obj, "limitPx");
    const auto qty = detail::req_str(obj, "sz");
    const auto orig = soft_str(obj, "origSz");
    const auto cloid = soft_str(obj, "cloid");
    const auto order_type = soft_str(obj, "orderType");
    const auto tif = soft_str(obj, "tif");
    const auto oid = detail::opt_u64(obj, "oid");
    const auto ts = detail::opt_ms(obj, "timestamp");
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

} // namespace

std::expected<std::span<const WireSpotAsset>, WireError> parse_spot_meta(std::string_view body,
                                                                         SpotAssetBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotAsset>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }

        // Token index -> (name, szDecimals). Indices are dense and small today
        // but the join is by VALUE, not by position: a sparse tokens array
        // would otherwise attach one token's precision to another's pair.
        struct Token {
            std::string_view name;
            std::int64_t sz_decimals{0};
            bool present{false};
        };
        static thread_local std::array<Token, k_max_spot_assets> tokens{};
        tokens.fill(Token{});

        auto token_rows = detail::req_array(*root, "tokens");
        if (!token_rows.has_value()) {
            return std::unexpected(token_rows.error());
        }
        for (auto row : *token_rows) {
            detail::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto name = detail::req_str(obj, "name");
            const auto decimals = detail::opt_i64(obj, "szDecimals");
            const auto index = detail::opt_i64(obj, "index");
            if (!name.has_value() || !decimals.has_value() || !index.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (*index < 0 || static_cast<std::size_t>(*index) >= tokens.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            tokens[static_cast<std::size_t>(*index)] = {
                .name = *name, .sz_decimals = *decimals, .present = true};
        }

        auto pairs = detail::req_array(*root, "universe");
        if (!pairs.has_value()) {
            return std::unexpected(pairs.error());
        }
        std::size_t count = 0;
        for (auto row : *pairs) {
            if (count == k_max_spot_assets) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto name = detail::req_str(obj, "name");
            const auto index = detail::opt_i64(obj, "index");
            if (!name.has_value() || !index.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            auto members = detail::req_array(obj, "tokens");
            if (!members.has_value()) {
                return std::unexpected(members.error());
            }
            std::int64_t base = -1;
            for (auto member : *members) {
                std::int64_t value = 0;
                if (const auto ec = member.get_int64().get(value); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                base = value;
                break; // [base, quote]; the quote is USDC on every pair listed
            }
            if (base < 0 || static_cast<std::size_t>(base) >= tokens.size() ||
                !tokens[static_cast<std::size_t>(base)].present) {
                continue; // a pair whose base token this body did not carry
            }
            const auto& token = tokens[static_cast<std::size_t>(base)];
            storage.assets[count] = {.name = *name,
                                     .base = token.name,
                                     .index = *index,
                                     .sz_decimals = token.sz_decimals};
            ++count;
        }
        return std::span<const WireSpotAsset>{storage.assets.data(), count};
    });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePosition>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = detail::req_array(*root, "assetPositions");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto row : *rows) {
            if (count == k_max_positions) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object wrapper;
            if (const auto ec = row.get_object().get(wrapper); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            // The `type` beside it ("oneWay") is the venue's position mode and
            // the domain has no other; reading it would only invite a switch.
            auto position = detail::req_object(wrapper, "position");
            if (!position.has_value()) {
                return std::unexpected(position.error());
            }
            const auto coin = detail::req_str(*position, "coin");
            const auto size = detail::req_str(*position, "szi");
            const auto entry = detail::req_str(*position, "entryPx");
            if (!coin.has_value() || !size.has_value() || !entry.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.positions[count] = {.coin = *coin, .size = *size, .entry_px = *entry};
            ++count;
        }
        // After the array, never before: On-Demand cannot revisit a key it has
        // already walked past, and `time` trails assetPositions in the body.
        const auto ts = detail::opt_ms(*root, "time");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        storage.ts_ms = *ts;
        return std::span<const WirePosition>{storage.positions.data(), count};
    });
}

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
            detail::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
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
            detail::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireFill out{};
            const auto coin = detail::req_str(obj, "coin");
            const auto price = detail::req_str(obj, "px");
            const auto qty = detail::req_str(obj, "sz");
            const auto side = detail::req_str(obj, "side");
            const auto fee = detail::req_str(obj, "fee");
            const auto fee_token = soft_str(obj, "feeToken");
            const auto cloid = soft_str(obj, "cloid");
            const auto oid = detail::opt_u64(obj, "oid");
            const auto tid = detail::opt_u64(obj, "tid");
            const auto ts = detail::opt_ms(obj, "time");
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
        const auto outer = detail::req_str(*root, "status");
        if (!outer.has_value()) {
            return std::unexpected(outer.error());
        }
        WireOrderStatus out{};
        if (*outer != "order") {
            return out; // "unknownOid": not found, and not an error either
        }
        auto wrapper = detail::req_object(*root, "order");
        if (!wrapper.has_value()) {
            return std::unexpected(wrapper.error());
        }
        auto inner = detail::req_object(*wrapper, "order");
        if (!inner.has_value()) {
            return std::unexpected(inner.error());
        }
        const auto order = read_open_order(*inner);
        if (!order.has_value()) {
            return std::unexpected(order.error());
        }
        // Read AFTER the nested object: On-Demand cannot revisit a field it
        // has already walked past, and `status` here shadows the outer one.
        const auto status = detail::req_str(*wrapper, "status");
        const auto ts = detail::opt_ms(*wrapper, "statusTimestamp");
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

namespace {

[[nodiscard]] ExchangeOutcome outcome_from_word(std::string_view word) noexcept {
    if (word == "success") {
        return ExchangeOutcome::Success;
    }
    // A TP/SL order answers with this bare string rather than an object, so a
    // reader that assumes every element is an object drops it as malformed.
    if (word == "waitingForTrigger") {
        return ExchangeOutcome::WaitingForTrigger;
    }
    return ExchangeOutcome::Unknown;
}

std::expected<WireExchangeStatus, WireError> read_status(detail::ondemand::value entry) {
    detail::ondemand::json_type kind{};
    if (const auto ec = entry.type().get(kind); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireExchangeStatus out{};
    if (kind == detail::ondemand::json_type::string) {
        const auto word = detail::str_of(entry);
        if (!word.has_value()) {
            return std::unexpected(word.error());
        }
        out.outcome = outcome_from_word(*word);
        return out;
    }
    detail::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    // The outcome IS the key, and there is exactly one. Walking the fields is
    // the only way to read it: On-Demand cannot try one name and then another.
    for (auto field : obj) {
        std::string_view key;
        if (const auto ec = field.unescaped_key().get(key); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (key == "error") {
            const auto text = detail::str_of(field.value());
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            out.outcome = ExchangeOutcome::Error;
            out.error = *text;
            return out;
        }
        detail::ondemand::object inner;
        if (const auto ec = field.value().get_object().get(inner); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (key == "resting") {
            const auto oid = detail::opt_u64(inner, "oid");
            if (!oid.has_value()) {
                return std::unexpected(oid.error());
            }
            out.outcome = ExchangeOutcome::Resting;
            out.oid = *oid;
            return out;
        }
        if (key == "filled") {
            const auto qty = detail::req_str(inner, "totalSz");
            const auto price = detail::req_str(inner, "avgPx");
            const auto oid = detail::opt_u64(inner, "oid");
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

std::expected<WireExchangeReply, WireError> parse_exchange_reply(std::string_view body,
                                                                 ExchangeStatusBuffer& storage) {
    return guarded([&]() -> std::expected<WireExchangeReply, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = detail::req_str(*root, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        WireExchangeReply out{};
        out.ok = *status == "ok";

        detail::ondemand::value response;
        if (const auto ec = (*root)["response"].get(response); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        detail::ondemand::json_type kind{};
        if (const auto ec = response.type().get(kind); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (kind == detail::ondemand::json_type::string) {
            // The same key, another type: a top-level refusal answers prose.
            const auto text = detail::str_of(response);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            out.message = *text;
            return out;
        }

        detail::ondemand::object envelope;
        if (const auto ec = response.get_object().get(envelope); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto type = detail::req_str(envelope, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        out.type = *type;

        auto data = detail::req_object(envelope, "data");
        if (!data.has_value()) {
            // Several actions answer {"type":"default"} with nothing beside it.
            return data.error() == WireError::MissingField
                       ? std::expected<WireExchangeReply, WireError>{out}
                       : std::unexpected(data.error());
        }
        auto rows = detail::req_array(*data, "statuses");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto row : *rows) {
            if (count == k_max_statuses) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::value entry;
            if (const auto ec = row.get(entry); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
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
