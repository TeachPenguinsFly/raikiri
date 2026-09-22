#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::kucoin {

namespace {

using wire::guarded;
using wire::session;

// `subject` separates all five shapes, including the two sharing the
// /contract/instrument topic, so the topic is read only for the symbol.
EventKind kind_from_subject(std::string_view subject) noexcept {
    if (subject == "tickerV2" || subject == "trade.ticker") {
        return EventKind::Bbo;
    }
    if (subject == "match" || subject == "trade.l3match") {
        return EventKind::Trade;
    }
    // "level2" is BOTH trees' subject and two different payloads: a packed
    // single-level change on futures, a whole book on spot. Only the socket
    // knows which, which is why the parsers take a market.
    if (subject == "level2") {
        return EventKind::Depth;
    }
    if (subject == "mark.index.price") {
        return EventKind::MarkPrice;
    }
    if (subject == "funding.rate") {
        return EventKind::FundingRate;
    }
    return EventKind::Unknown;
}

// A batched subscription is echoed one symbol per frame (verified live), so
// this is never a list.
std::string_view symbol_of_topic(std::string_view topic) noexcept {
    const auto colon = topic.rfind(':');
    if (colon == std::string_view::npos) {
        return {};
    }
    topic.remove_prefix(colon + 1); // not substr: it throws, and this is noexcept
    return topic;
}

// Shared body of the opt_/req_ pairs; an absent field reads as empty.
template <typename Read>
[[nodiscard]] std::expected<std::string_view, WireError>
lookup(wire::ondemand::object& obj, std::string_view key, Read read) {
    wire::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return read(value);
}

} // namespace

std::expected<std::string_view, WireError> topic_symbol(wire::ondemand::object& root) {
    const auto topic = wire::req_str(root, "topic");
    if (!topic.has_value()) {
        return std::unexpected(topic.error());
    }
    const auto symbol = symbol_of_topic(*topic);
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return symbol;
}

// raw_json() returns the token plus whitespace up to the next one. Contents
// stay escaped: KuCoin symbols, order ids and decimal strings cannot contain an
// escape, and letting simdjson unescape would move every field into a buffer
// that cannot be rebased.
std::expected<std::string_view, WireError> raw_of(wire::ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return session().rebase(wire::trim(raw));
}

std::expected<std::string_view, WireError> decimal_of(wire::ondemand::value value) {
    const auto raw = raw_of(value);
    if (!raw.has_value()) {
        return raw;
    }
    if (*raw == "null") {
        return std::string_view{};
    }
    if (raw->size() >= 2 && raw->front() == '"' && raw->back() == '"') {
        return raw->substr(1, raw->size() - 2);
    }
    return *raw;
}

std::int64_t i64_of_digits(std::string_view text) noexcept {
    std::int64_t out = 0;
    const auto* const end = text.data() + text.size();
    const auto [stop, ec] = std::from_chars(text.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : std::int64_t{0};
}

std::expected<std::string_view, WireError> opt_decimal(wire::ondemand::object& obj,
                                                       std::string_view key) {
    return lookup(obj, key, decimal_of);
}

std::expected<std::string_view, WireError> req_decimal(wire::ondemand::object& obj,
                                                       std::string_view key) {
    auto text = opt_decimal(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::expected<std::uint64_t, WireError> opt_u64(wire::ondemand::object& obj,
                                                std::string_view key) {
    const auto text = opt_decimal(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return wire::digits_to_u64(*text);
}

std::expected<std::int64_t, WireError> opt_i64(wire::ondemand::object& obj,
                                               std::string_view key) {
    const auto text = opt_decimal(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return i64_of_digits(*text);
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        using Text = std::expected<std::string_view, WireError>;
        Text type{std::unexpected(WireError::MissingField)};
        Text subject{std::unexpected(WireError::MissingField)};
        Text topic{std::unexpected(WireError::MissingField)};
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view key, wire::ondemand::value& value) {
                if (key == "type") {
                    type = wire::str_of(value);
                } else if (key == "subject") {
                    subject = wire::str_of(value);
                } else if (key == "topic") {
                    topic = wire::str_of(value);
                }
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        for (auto* text : {&type, &subject, &topic}) {
            if (text->has_value() && (*text)->empty()) {
                *text = std::unexpected(WireError::MissingField);
            }
        }
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        EventInfo info{};
        if (*type != "message") {
            if (*type == "welcome") {
                info.kind = EventKind::Welcome;
            } else if (*type == "ack") {
                info.kind = EventKind::Ack;
            } else if (*type == "pong") {
                info.kind = EventKind::Pong;
            } else if (*type == "error") {
                info.kind = EventKind::Error;
            }
            return info;
        }
        if (!subject.has_value()) {
            return std::unexpected(subject.error());
        }
        info.kind = kind_from_subject(*subject);
        if (!topic.has_value()) {
            return std::unexpected(topic.error());
        }
        info.symbol = symbol_of_topic(*topic);
        if (info.symbol.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return info;
    });
}

std::expected<EventInfo, WireError> parse_private_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = wire::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        EventInfo info{};
        if (*type != "message") {
            if (*type == "welcome") {
                info.kind = EventKind::Welcome;
            } else if (*type == "ack") {
                info.kind = EventKind::Ack;
            } else if (*type == "pong") {
                info.kind = EventKind::Pong;
            } else if (*type == "error") {
                info.kind = EventKind::Error;
            }
            return info;
        }
        const auto subject = wire::req_str(*root, "subject");
        if (!subject.has_value()) {
            return std::unexpected(subject.error());
        }
        // The all-symbols topic says orderChange; the per-symbol form of the
        // same futures topic says symbolOrderChange, and the payloads match.
        if (*subject == "orderChange" || *subject == "symbolOrderChange") {
            info.kind = EventKind::OrderChange;
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
        const auto code = opt_i64(*root, "code");
        const auto msg = wire::opt_str(*root, "data");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .msg = *msg};
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
        if (value == 0) {
            return std::unexpected(WireError::WrongType);
        }
        return RestEnvelope{.code = static_cast<std::int64_t>(value), .msg = *msg};
    });
}

std::expected<WireBullet, WireError> parse_bullet(std::string_view body) {
    return guarded([&]() -> std::expected<WireBullet, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [](wire::ondemand::object& obj) -> std::expected<WireBullet, WireError> {
                WireBullet out{};
                const auto token = wire::req_str(obj, "token");
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                out.token = *token;
                auto servers = wire::req_array(obj, "instanceServers");
                if (!servers.has_value()) {
                    return std::unexpected(servers.error());
                }
                for (auto entry : *servers) {
                    wire::ondemand::object server;
                    if (const auto ec = entry.get_object().get(server); ec != simdjson::SUCCESS) {
                        return std::unexpected(wire::map_error(ec));
                    }
                    const auto endpoint = wire::req_str(server, "endpoint");
                    const auto interval = opt_u64(server, "pingInterval");
                    const auto timeout = opt_u64(server, "pingTimeout");
                    if (!endpoint.has_value() || !interval.has_value() || !timeout.has_value()) {
                        return std::unexpected(WireError::MissingField);
                    }
                    out.endpoint = *endpoint;
                    out.ping_interval_ms = *interval;
                    out.ping_timeout_ms = *timeout;
                    return out;
                }
                return std::unexpected(WireError::MissingField);
            });
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = opt_u64(*root, "data");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::kucoin
