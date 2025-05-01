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
    if (type == "subscriptions") {
        return EventKind::Subscriptions;
    }
    if (type == "error") {
        return EventKind::Error;
    }
    return EventKind::Unknown;
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

} // namespace raikiri::venues::coinbase
