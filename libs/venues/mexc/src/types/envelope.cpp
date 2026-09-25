#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/wire/json.hpp>
#include <raikiri/wire/protobuf.hpp>

namespace raikiri::venues::mexc {

namespace {

namespace pb = raikiri::wire::pb;

using wire::guarded;
using wire::session;

constexpr std::uint32_t k_field_channel = 1;
constexpr std::uint32_t k_field_symbol = 3;
constexpr std::uint32_t k_field_send_ms = 6;

// the present payload field number is the frame type
constexpr std::uint32_t k_field_depth = 303;
constexpr std::uint32_t k_field_deals = 314;
constexpr std::uint32_t k_field_book_ticker = 315;

StreamKind kind_of(std::uint32_t number) noexcept {
    switch (number) {
    case k_field_book_ticker:
        return StreamKind::Quote;
    case k_field_deals:
        return StreamKind::Trade;
    case k_field_depth:
        return StreamKind::Depth;
    default:
        return StreamKind::Unknown;
    }
}

} // namespace

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "push.ticker") {
        return EventKind::Ticker;
    }
    if (channel == "push.deal") {
        return EventKind::Trade;
    }
    if (channel == "push.depth.full") {
        return EventKind::DepthFull;
    }
    if (channel == "push.depth") {
        return EventKind::Depth;
    }
    if (channel == "pong") {
        return EventKind::Pong;
    }
    if (channel == "push.personal.order") {
        return EventKind::PersonalOrder;
    }
    if (channel == "push.personal.order.deal") {
        return EventKind::PersonalDeal;
    }
    if (channel == "push.personal.position") {
        return EventKind::PersonalPosition;
    }
    if (channel == "rs.login") {
        return EventKind::LoginAck;
    }
    if (channel == "rs.error") {
        return EventKind::Error;
    }
    if (channel.starts_with("rs.")) {
        return EventKind::SubscribeAck;
    }
    return EventKind::Unknown;
}

std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value) {
    // raw_json() includes trailing whitespace; contents stay escaped
    // since mexc tokens contain no escapes
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    raw = wire::trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw = raw.substr(1, raw.size() - 2);
    } else if (raw.empty() || raw.front() == '{' || raw.front() == '[') {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw);
}

std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj, std::string_view key) {
    wire::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return scalar_of(value);
}

std::expected<std::string_view, WireError> req_scalar(wire::ondemand::object& obj, std::string_view key) {
    wire::ondemand::value value;
    if (const auto ec = wire::find(obj, key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    auto text = scalar_of(value);
    if (text && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::expected<std::int64_t, WireError>
opt_int(wire::ondemand::object& obj, std::string_view key, std::int64_t fallback) {
    std::int64_t out = 0;
    const auto ec = wire::find(obj, key).get_int64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return fallback;
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return out;
}

std::expected<std::uint64_t, WireError> scalar_ms(wire::ondemand::object& obj, std::string_view key) {
    const auto text = opt_scalar(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return wire::digits_to_u64(*text);
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        const auto channel = req_scalar(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        return EventInfo{.kind = kind_from_channel(*channel), .symbol = *symbol};
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto msg = req_scalar(*root, "data");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return StreamError{.msg = *msg};
    });
}

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        bool success = false;
        if (const auto ec = wire::find(*root, "success").get_bool().get(success); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto code = opt_int(*root, "code", -1);
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        if (*code < 0) {
            return std::unexpected(WireError::MissingField);
        }
        // errors carry `message`; successes omit it
        const auto msg = opt_scalar(*root, "message");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return RestEnvelope{.success = success, .code = *code, .msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = scalar_ms(*root, "data");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

std::expected<SpotError, WireError> parse_spot_error(std::string_view body) {
    return guarded([&]() -> std::expected<SpotError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = opt_int(*root, "code", 0);
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        root->reset();
        const auto msg = opt_scalar(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return SpotError{.code = *code, .msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = scalar_ms(*root, "serverTime");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

std::expected<StreamEvent, WireError> parse_stream_event(std::string_view frame) {
    StreamEvent out{};
    bool named = false;
    pb::Reader reader{frame};
    while (const auto entry = reader.next()) {
        if (entry->type == pb::WireType::Varint) {
            if (entry->number == k_field_send_ms) {
                out.ts_ms = entry->varint;
            }
            continue;
        }
        if (entry->type != pb::WireType::Bytes) {
            continue;
        }
        switch (entry->number) {
        case k_field_channel:
            named = true;
            break;
        case k_field_symbol:
            out.symbol = entry->bytes;
            break;
        default: {
            const auto kind = kind_of(entry->number);
            if (kind != StreamKind::Unknown) {
                out.kind = kind;
                out.payload = entry->bytes;
            }
            break;
        }
        }
    }
    if (!named || out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

} // namespace raikiri::venues::mexc
