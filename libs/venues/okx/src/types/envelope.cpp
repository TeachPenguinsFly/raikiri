#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx {

namespace {

using wire::guarded;
using wire::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "bbo-tbt") {
        return EventKind::Bbo;
    }
    if (channel == "trades") {
        return EventKind::Trade;
    }
    if (channel == "funding-rate") {
        return EventKind::FundingRate;
    }
    if (channel == "mark-price") {
        return EventKind::MarkPrice;
    }
    if (channel == "tickers") {
        return EventKind::Tickers;
    }
    if (channel == "books") {
        return EventKind::Books;
    }
    if (channel == "orders") {
        return EventKind::Orders;
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
        const auto event = wire::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        EventInfo info{};
        if (!event->empty()) {
            // A login answer carries a code that is 0 on success and an auth
            // failure otherwise, so it cannot share the subscribe path: a
            // refused login read as an ack leaves a socket that will answer
            // every subscribe with 60011 and no explanation.
            info.kind = *event == "error"   ? EventKind::Error
                        : *event == "login" ? EventKind::LoginAck
                                            : EventKind::SubscribeAck;
            return info;
        }
        auto arg = wire::req_object(*root, "arg");
        if (!arg.has_value()) {
            return std::unexpected(arg.error());
        }
        const auto channel = wire::req_str(*arg, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        info.kind = kind_from_channel(*channel);
        const auto inst_id = wire::opt_str(*arg, "instId");
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
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
        const auto msg = wire::opt_str(*root, "msg");
        const auto code = wire::opt_str(*root, "code");
        if (!msg.has_value() || !code.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = static_cast<std::int64_t>(wire::digits_to_u64(*code)),
                           .msg = *msg};
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

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_first_data(
            *root, [](wire::ondemand::object& obj) -> std::expected<std::uint64_t, WireError> {
                const auto ts = wire::opt_ms(obj, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                if (*ts == 0) {
                    return std::unexpected(WireError::MissingField);
                }
                return *ts;
            });
    });
}

} // namespace raikiri::venues::okx
