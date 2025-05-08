#include "wire_detail.hpp"

namespace raikiri::venues::coinbase {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_type(std::string_view type) noexcept {
    if (type == "ticker") {
        return EventKind::Ticker;
    }
    if (type == "match" || type == "last_match") {
        return EventKind::Match;
    }
    if (type == "snapshot") {
        return EventKind::Snapshot;
    }
    if (type == "l2update") {
        return EventKind::L2Update;
    }
    if (type == "subscriptions") {
        return EventKind::Subscriptions;
    }
    if (type == "error") {
        return EventKind::Error;
    }
    return EventKind::Unknown;
}

// ["price","size"], both decimal strings. Reading stops at the buffer rather
// than failing: see wire.hpp on why truncating this particular array is safe.
std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& root, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            break;
        }
        detail::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        std::size_t field = 0;
        for (auto element : pair) {
            detail::ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            // str_of, never get_string: the unescaping reader writes into a
            // buffer that cannot be rebased onto the caller's frame, and what
            // comes back from that is an empty view rather than a price.
            const auto text = detail::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            if (field == 0) {
                into[count].price = *text;
            } else if (field == 1) {
                into[count].qty = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected(WireError::ShortArray);
        }
        ++count;
    }
    return count;
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = detail::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        EventInfo info{};
        info.kind = kind_from_type(*type);
        const auto product = detail::opt_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        info.product_id = *product;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto message = detail::req_str(*root, "message");
        const auto reason = detail::opt_str(*root, "reason");
        if (!message.has_value() || !reason.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return StreamError{.message = *message, .reason = *reason};
    });
}

std::expected<std::span<const std::string_view>, WireError>
parse_subscriptions(std::string_view frame, AckBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const std::string_view>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "channels");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_ack_channels) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto name = detail::req_str(obj, "name");
            if (!name.has_value()) {
                return std::unexpected(name.error());
            }
            storage.channels[count] = *name;
            ++count;
        }
        return std::span<const std::string_view>{storage.channels.data(), count};
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTicker out{};
        const auto product = detail::req_str(*root, "product_id");
        const auto bid = detail::req_str(*root, "best_bid");
        const auto bid_size = detail::req_str(*root, "best_bid_size");
        const auto ask = detail::req_str(*root, "best_ask");
        const auto ask_size = detail::req_str(*root, "best_ask_size");
        const auto time = detail::req_str(*root, "time");
        if (!product.has_value() || !bid.has_value() || !bid_size.has_value() || !ask.has_value() ||
            !ask_size.has_value() || !time.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.product_id = *product;
        out.best_bid = *bid;
        out.best_bid_size = *bid_size;
        out.best_ask = *ask;
        out.best_ask_size = *ask_size;
        out.time = *time;
        const auto sequence = detail::opt_i64(*root, "sequence");
        const auto trade_id = detail::opt_i64(*root, "trade_id");
        if (!sequence.has_value() || !trade_id.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.sequence = *sequence;
        out.trade_id = *trade_id;
        return out;
    });
}

std::expected<WireMatch, WireError> parse_match(std::string_view frame) {
    return guarded([&]() -> std::expected<WireMatch, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireMatch out{};
        const auto product = detail::req_str(*root, "product_id");
        const auto price = detail::req_str(*root, "price");
        const auto size = detail::req_str(*root, "size");
        const auto time = detail::req_str(*root, "time");
        const auto side = detail::req_str(*root, "side");
        if (!product.has_value() || !price.has_value() || !size.has_value() || !time.has_value() ||
            !side.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.product_id = *product;
        out.price = *price;
        out.size = *size;
        out.time = *time;
        out.maker_sold = *side == "sell";
        const auto sequence = detail::opt_i64(*root, "sequence");
        const auto trade_id = detail::opt_i64(*root, "trade_id");
        if (!sequence.has_value() || !trade_id.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.sequence = *sequence;
        out.trade_id = *trade_id;
        return out;
    });
}

std::expected<WireL2Snapshot, WireError> parse_l2_snapshot(std::string_view frame,
                                                           LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireL2Snapshot, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto product = detail::req_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        WireL2Snapshot out{};
        out.product_id = *product;
        const auto bids = read_levels(*root, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*root, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto time = detail::opt_str(*root, "time");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        out.time = *time;
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

std::expected<WireL2Update, WireError> parse_l2_update(std::string_view frame,
                                                       ChangeBuffer& storage) {
    return guarded([&]() -> std::expected<WireL2Update, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto product = detail::req_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        WireL2Update out{};
        out.product_id = *product;
        auto array = detail::req_array(*root, "changes");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_changes) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::array triple;
            if (const auto ec = entry.get_array().get(triple); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireChange change{};
            std::size_t field = 0;
            for (auto element : triple) {
                detail::ondemand::value value;
                if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                const auto text = detail::str_of(value);
                if (!text.has_value()) {
                    return std::unexpected(text.error());
                }
                if (field == 0) {
                    change.bid = *text == "buy";
                } else if (field == 1) {
                    change.price = *text;
                } else if (field == 2) {
                    change.qty = *text;
                }
                ++field;
            }
            if (field < 3) {
                return std::unexpected(WireError::ShortArray);
            }
            storage.changes[count] = change;
            ++count;
        }
        const auto time = detail::opt_str(*root, "time");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        out.time = *time;
        out.changes = {storage.changes.data(), count};
        return out;
    });
}

} // namespace raikiri::venues::coinbase
