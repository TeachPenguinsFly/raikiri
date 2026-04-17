#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::blofin {

namespace {

using wire::guarded;
using wire::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "books") {
        return EventKind::Books;
    }
    if (channel == "trades") {
        return EventKind::Trades;
    }
    if (channel == "tickers") {
        return EventKind::Tickers;
    }
    if (channel == "funding-rate") {
        return EventKind::FundingRate;
    }
    return EventKind::Unknown;
}

std::size_t count_of(std::string_view text, std::string_view needle) noexcept {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// `msg` is read raw, so the echoed frame's quotes are still escaped.
std::string_view echoed_inst_id(std::string_view msg) noexcept {
    constexpr std::string_view key = R"(instId\":\")";
    const auto at = msg.find(key);
    if (at == std::string_view::npos) {
        return {};
    }
    const auto from = at + key.size();
    const auto to = msg.find(R"(\")", from);
    return to == std::string_view::npos ? std::string_view{} : msg.substr(from, to - from);
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto event = wire::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        EventInfo info{};
        if (!event->empty()) {
            info.kind = *event == "error" ? EventKind::Error : EventKind::SubscribeAck;
            return info;
        }
        auto arg = wire::req_object(*root, "arg");
        if (!arg.has_value()) {
            return std::unexpected(arg.error());
        }
        const auto channel = wire::req_str(*arg, "channel");
        const auto inst_id = wire::opt_str(*arg, "instId");
        if (!channel.has_value() || !inst_id.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        info.kind = kind_from_channel(*channel);
        info.inst_id = *inst_id;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = wire::req_str(*root, "code");
        const auto msg = wire::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return StreamError{.code = static_cast<std::int64_t>(wire::digits_to_u64(*code)),
                           .msg = *msg,
                           .inst_id = echoed_inst_id(*msg),
                           .args = count_of(*msg, R"(channel\":\")")};
    });
}

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = wire::req_str(*root, "code");
        const auto msg = wire::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        const auto value = wire::digits_to_u64(*code);
        if (value == 0 && *code != "0") {
            return std::unexpected(WireError::WrongType);
        }
        return RestEnvelope{.code = static_cast<std::int64_t>(value), .msg = *msg};
    });
}

} // namespace raikiri::venues::blofin
