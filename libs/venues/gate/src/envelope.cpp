#include <raikiri/venues/gate/envelope.hpp>

#include <charconv>

namespace raikiri::venues::gate {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// raw_json() on a scalar returns the token plus whitespace up to the next one.
// Contents stay escaped: Gate contract names and decimal strings cannot
// contain an escape, and letting simdjson unescape would move every field into
// a buffer that cannot be rebased.
std::expected<std::string_view, WireError> token_of(detail::ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return detail::trim(raw);
}

template <typename Fn>
std::expected<std::string_view, WireError>
find_scalar(detail::ondemand::object& obj, std::string_view key, bool required, Fn&& read) {
    detail::ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        if (required) {
            return std::unexpected(WireError::MissingField);
        }
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    auto text = std::forward<Fn>(read)(value);
    if (required && text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "futures.book_ticker") {
        return EventKind::BookTicker;
    }
    if (channel == "futures.trades") {
        return EventKind::Trade;
    }
    if (channel == "futures.tickers") {
        return EventKind::Ticker;
    }
    if (channel == "futures.order_book_update") {
        return EventKind::BookUpdate;
    }
    if (channel == "spot.book_ticker") {
        return EventKind::BookTicker;
    }
    if (channel == "spot.trades") {
        return EventKind::Trade;
    }
    if (channel == "spot.order_book") {
        return EventKind::BookSnapshot;
    }
    if (channel == "futures.orders" || channel == "spot.orders") {
        return EventKind::OrderUpdate;
    }
    if (channel == "futures.usertrades" || channel == "spot.usertrades") {
        return EventKind::UserTrade;
    }
    return EventKind::Unknown;
}

// The echoed subscription payload. Its first element is the contract on every
// channel; the depth channel appends an interval and a level count.
std::expected<std::string_view, WireError> first_payload(detail::ondemand::object& root) {
    auto array = detail::req_array(root, "payload");
    if (!array.has_value()) {
        return std::string_view{}; // unsubscribe-all frames carry none
    }
    for (auto entry : *array) {
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        return detail::str_of(value);
    }
    return std::string_view{};
}

} // namespace

std::expected<std::string_view, WireError> num_of(detail::ondemand::value value) {
    const auto raw = token_of(value);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (*raw == "null") {
        return std::string_view{};
    }
    if (raw->size() >= 2 && raw->front() == '"' && raw->back() == '"') {
        return session().rebase(raw->substr(1, raw->size() - 2));
    }
    const char lead = raw->front();
    if (lead != '-' && lead != '+' && (lead < '0' || lead > '9')) {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(*raw);
}

std::expected<std::string_view, WireError> opt_num(detail::ondemand::object& obj,
                                                   std::string_view key) {
    return find_scalar(obj, key, false, num_of);
}

std::expected<std::string_view, WireError> req_num(detail::ondemand::object& obj,
                                                   std::string_view key) {
    return find_scalar(obj, key, true, num_of);
}

std::uint64_t whole_u64(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    std::uint64_t out = 0;
    const auto* const end = s.data() + s.size();
    const auto [stop, ec] = std::from_chars(s.data(), end, out);
    if (ec != std::errc{}) {
        return 0;
    }
    if (stop == end) {
        return out;
    }
    return *stop == '.' ? out : 0;
}

std::expected<std::uint64_t, WireError> num_u64(detail::ondemand::object& obj,
                                                std::string_view key) {
    const auto text = opt_num(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return whole_u64(*text);
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = detail::req_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        EventInfo info{};
        if (*channel == "futures.pong") {
            info.kind = EventKind::Pong;
            return info;
        }
        const auto event = detail::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        if (*event == "update") {
            info.kind = kind_from_channel(*channel);
            return info;
        }
        const auto contract = first_payload(*root);
        if (!contract.has_value()) {
            return std::unexpected(contract.error());
        }
        info.contract = *contract;
        detail::ondemand::object error;
        const auto ec = root->find_field_unordered("error").get_object().get(error);
        info.kind = ec == simdjson::SUCCESS ? EventKind::Error : EventKind::SubscribeAck;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto error = detail::req_object(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        std::int64_t code = 0;
        if (const auto ec = error->find_field_unordered("code").get_int64().get(code);
            ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto message = detail::opt_str(*error, "message");
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        return StreamError{.code = code, .message = *message};
    });
}

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto label = detail::req_str(*root, "label");
        if (!label.has_value()) {
            return std::unexpected(label.error());
        }
        const auto message = detail::opt_str(*root, "message");
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        return RestError{.label = *label, .message = *message};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ms = num_u64(*root, "server_time");
        if (!ms.has_value()) {
            return std::unexpected(ms.error());
        }
        if (*ms == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ms;
    });
}

} // namespace raikiri::venues::gate
