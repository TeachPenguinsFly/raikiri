#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// The part of a topic after the contract code: "market.BTC-USDT.trade.detail"
// -> "trade.detail". Empty when the shape does not hold.
std::string_view suffix_of(std::string_view topic) {
    const auto first = topic.find('.');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto second = topic.find('.', first + 1);
    if (second == std::string_view::npos) {
        return {};
    }
    return topic.substr(second + 1);
}

// The private topics carry ONE dot, so suffix_of finds nothing in them and
// kind_from_topic would call every order push Unknown. Both margin modes are
// listed because the `_cross` family is what this adapter subscribes to and
// the bare family is what an isolated-margin account would see.
EventKind private_kind_from_topic(std::string_view topic) noexcept {
    if (topic.starts_with("orders_cross.") || topic.starts_with("orders.")) {
        return EventKind::PrivateOrder;
    }
    if (topic.starts_with("positions_cross.") || topic.starts_with("positions.")) {
        return EventKind::PrivatePosition;
    }
    if (topic.starts_with("matchOrders_cross.") || topic.starts_with("matchOrders.")) {
        return EventKind::PrivateMatch;
    }
    return EventKind::Unknown;
}

EventKind kind_from_topic(std::string_view topic) noexcept {
    const auto suffix = suffix_of(topic);
    if (suffix == "bbo") {
        return EventKind::Bbo;
    }
    if (suffix == "trade.detail") {
        return EventKind::Trade;
    }
    if (suffix == "depth.step0") {
        return EventKind::Depth;
    }
    // "detail" only: the trade channel's suffix is "trade.detail" and reaches
    // the compare above it.
    if (suffix == "detail") {
        return EventKind::Detail;
    }
    if (suffix == "funding_rate") {
        return EventKind::Funding;
    }
    return EventKind::Unknown;
}

} // namespace

std::string_view unquote(std::string_view raw) noexcept {
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return raw.substr(1, raw.size() - 2);
    }
    return raw;
}

std::expected<std::string_view, WireError> num_of(detail::ondemand::object& obj,
                                                  std::string_view key) {
    const auto raw = detail::opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    const auto text = unquote(*raw);
    return text == "null" ? std::string_view{} : text;
}

std::expected<std::uint64_t, WireError> opt_ms_str(detail::ondemand::object& obj,
                                                   std::string_view key) {
    const auto text = detail::opt_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return detail::digits_to_u64(*text);
}

std::string_view code_of(std::string_view topic) {
    const auto first = topic.find('.');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto second = topic.find('.', first + 1);
    if (second == std::string_view::npos) {
        return {};
    }
    return topic.substr(first + 1, second - first - 1);
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        EventInfo info{};

        const auto op = detail::opt_str(*root, "op");
        if (!op.has_value()) {
            return std::unexpected(op.error());
        }
        if (!op->empty()) {
            if (*op == "ping") {
                const auto ts = detail::opt_raw(*root, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                info.kind = EventKind::NotifyPing;
                info.keepalive_ts = *ts;
                return info;
            }
            if (*op == "auth") {
                info.kind = EventKind::AuthReply;
                return info;
            }
            const auto topic = detail::opt_str(*root, "topic");
            if (!topic.has_value()) {
                return std::unexpected(topic.error());
            }
            info.contract_code = code_of(*topic);
            if (*op == "notify") {
                info.kind = kind_from_topic(*topic);
                if (info.kind == EventKind::Unknown) {
                    info.kind = private_kind_from_topic(*topic);
                }
                return info;
            }
            const auto code = detail::opt_raw(*root, "err-code");
            if (!code.has_value()) {
                return std::unexpected(code.error());
            }
            info.kind = (*op == "sub" && *code == "0") ? EventKind::SubscribeAck : EventKind::Error;
            return info;
        }

        // The spot v2 socket speaks `action` where every other HTX endpoint
        // speaks `op`, and its keepalive hides the timestamp one level down.
        const auto action = detail::opt_str(*root, "action");
        if (!action.has_value()) {
            return std::unexpected(action.error());
        }
        if (!action->empty()) {
            if (*action == "ping") {
                auto data = detail::req_object(*root, "data");
                if (!data.has_value()) {
                    return std::unexpected(data.error());
                }
                const auto ts = detail::opt_raw(*data, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                info.kind = EventKind::SpotPing;
                info.keepalive_ts = *ts;
                return info;
            }
            const auto channel = detail::opt_str(*root, "ch");
            if (!channel.has_value()) {
                return std::unexpected(channel.error());
            }
            if (*channel == "auth") {
                info.kind = EventKind::AuthReply;
                return info;
            }
            if (*action == "push") {
                if (channel->starts_with("orders#")) {
                    info.kind = EventKind::PrivateOrder;
                } else if (channel->starts_with("trade.clearing#")) {
                    info.kind = EventKind::PrivateMatch;
                }
                return info;
            }
            const auto code = detail::opt_i64(*root, "code");
            if (!code.has_value()) {
                return std::unexpected(code.error());
            }
            info.kind = *code == 200 ? EventKind::SubscribeAck : EventKind::Error;
            return info;
        }

        const auto ch = detail::opt_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        if (!ch->empty()) {
            info.kind = kind_from_topic(*ch);
            info.contract_code = code_of(*ch);
            if (info.kind == EventKind::Unknown) {
                if (ch->starts_with("orders#")) {
                    info.kind = EventKind::PrivateOrder;
                } else if (ch->starts_with("trade.clearing#")) {
                    info.kind = EventKind::PrivateMatch;
                }
            }
            return info;
        }

        const auto ping = detail::opt_raw(*root, "ping");
        if (!ping.has_value()) {
            return std::unexpected(ping.error());
        }
        if (!ping->empty()) {
            info.kind = EventKind::MarketPing;
            info.keepalive_ts = *ping;
            return info;
        }

        const auto status = detail::opt_str(*root, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        if (*status == "error") {
            info.kind = EventKind::Error;
            return info;
        }
        if (*status == "ok") {
            info.kind = EventKind::SubscribeAck;
            return info;
        }
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::opt_raw(*root, "err-code");
        const auto msg = detail::opt_str(*root, "err-msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (code->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return StreamError{.code = unquote(*code), .msg = *msg};
    });
}

std::expected<WireAuthReply, WireError> parse_auth_reply(std::string_view frame) {
    return guarded([&]() -> std::expected<WireAuthReply, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto op = detail::opt_str(*root, "op");
        if (!op.has_value()) {
            return std::unexpected(op.error());
        }
        if (*op == "auth") {
            const auto code = detail::opt_raw(*root, "err-code");
            const auto msg = detail::opt_str(*root, "err-msg");
            if (!code.has_value() || !msg.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto text = unquote(*code);
            return WireAuthReply{.ok = text == "0", .code = text, .msg = *msg};
        }
        // The spot v2 socket answers a REQUEST rather than an op, and states
        // success as HTTP 200 rather than as a zero error code.
        const auto code = detail::opt_i64(*root, "code");
        const auto raw = detail::opt_raw(*root, "code");
        const auto msg = detail::opt_str(*root, "message");
        if (!code.has_value() || !raw.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireAuthReply{.ok = *code == 200, .code = unquote(*raw), .msg = *msg};
    });
}

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = detail::opt_str(*root, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        if (*status == "ok") {
            return RestEnvelope{.ok = true, .code = {}, .msg = {}};
        }
        if (*status != "error") {
            return std::unexpected(WireError::WrongType);
        }
        auto code = detail::opt_raw(*root, "err_code");
        auto msg = detail::opt_str(*root, "err_msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (code->empty()) {
            code = detail::opt_raw(*root, "err-code");
            msg = detail::opt_str(*root, "err-msg");
            if (!code.has_value() || !msg.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
        }
        if (code->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return RestEnvelope{.ok = false, .code = unquote(*code), .msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = detail::opt_str(*root, "status");
        if (!status.has_value() || *status != "ok") {
            return std::unexpected(WireError::MissingField);
        }
        const auto ts = detail::opt_u64(*root, "ts");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::htx
