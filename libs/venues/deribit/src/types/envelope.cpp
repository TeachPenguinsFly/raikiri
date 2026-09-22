#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <charconv>

namespace raikiri::venues::deribit {

namespace {

using wire::guarded;
using wire::session;

// The parameter count distinguishes the two book streams:
// "book.BTC-PERPETUAL.100ms" is incremental, "book.BTC-PERPETUAL.none.1.100ms"
// a depth-limited snapshot. No instrument name contains a dot.
constexpr std::size_t k_max_segments = 6;

struct Channel {
    std::array<std::string_view, k_max_segments> parts{};
    std::size_t count{0};
};

Channel split_channel(std::string_view channel) noexcept {
    Channel out{};
    while (!channel.empty() && out.count < k_max_segments) {
        const auto dot = channel.find('.');
        if (dot == std::string_view::npos) {
            out.parts[out.count++] = channel;
            break;
        }
        out.parts[out.count++] = channel.substr(0, dot);
        channel.remove_prefix(dot + 1);
    }
    return out;
}

EventKind kind_from_channel(const Channel& channel) noexcept {
    if (channel.count < 3) {
        return EventKind::Unknown;
    }
    const auto stream = channel.parts[0];
    if (stream == "trades") {
        return EventKind::Trade;
    }
    if (stream == "ticker") {
        return EventKind::Ticker;
    }
    if (stream == "book") {
        return channel.count >= 5 ? EventKind::Bbo : EventKind::Book;
    }
    if (stream == "user") {
        // user.changes.{kind}.{currency}.raw — the instrument is NOT in the
        // channel here, it is in every row of the payload.
        return channel.parts[1] == "changes" ? EventKind::UserChange : EventKind::Unknown;
    }
    // Other channels exist; only what ws.cpp subscribes to is claimed here.
    return EventKind::Unknown;
}

// True where the channel's SECOND segment is an instrument name. On the
// private channels it is the topic, and copying it into EventInfo would hand
// the mapper "orders" as a symbol to look up.
bool names_instrument(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::Bbo:
    case EventKind::Trade:
    case EventKind::Ticker:
    case EventKind::Book:
        return true;
    case EventKind::UserChange:
    case EventKind::Heartbeat:
    case EventKind::Response:
    case EventKind::Unknown:
        return false;
    }
    return false;
}

// Shared spine of the opt_/req_ pairs; req_ adds a refusal of the empty case.
template <typename Reader>
[[nodiscard]] std::expected<std::string_view, WireError>
find_scalar(wire::ondemand::object& obj, std::string_view key, Reader reader) {
    wire::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return reader(value);
}

} // namespace

std::expected<wire::ondemand::object, WireError> push_params(wire::ondemand::object& root) {
    return wire::req_object(root, "params");
}

std::expected<wire::ondemand::object, WireError> push_data(wire::ondemand::object& root) {
    auto params = push_params(root);
    if (!params.has_value()) {
        return std::unexpected(params.error());
    }
    return wire::req_object(*params, "data");
}

std::expected<std::string_view, WireError> num_of(wire::ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    raw = wire::trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    // A quoted token here means Deribit changed a number into a string.
    if (raw.empty() || raw.front() == '"') {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw);
}

std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj,
                                                   std::string_view key) {
    return find_scalar(obj, key, [](wire::ondemand::value v) { return num_of(v); });
}

std::expected<std::string_view, WireError> req_num(wire::ondemand::object& obj,
                                                   std::string_view key) {
    const auto text = opt_num(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::uint64_t token_to_u64(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    std::uint64_t out = 0;
    const auto* const end = s.data() + s.size();
    const auto [stop, ec] = std::from_chars(s.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
}

std::expected<std::uint64_t, WireError> opt_ms(wire::ondemand::object& obj,
                                               std::string_view key) {
    const auto token = opt_num(obj, key);
    if (!token.has_value()) {
        return std::unexpected(token.error());
    }
    return token_to_u64(*token);
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto method = wire::opt_str(*root, "method");
        if (!method.has_value()) {
            return std::unexpected(method.error());
        }
        EventInfo info{};
        if (method->empty()) {
            info.kind = EventKind::Response; // a reply to something we sent
            return info;
        }
        auto params = push_params(*root);
        if (!params.has_value()) {
            return std::unexpected(params.error());
        }
        if (*method == "heartbeat") {
            const auto type = wire::req_str(*params, "type");
            if (!type.has_value()) {
                return std::unexpected(type.error());
            }
            info.kind = EventKind::Heartbeat;
            info.needs_test_reply = *type == "test_request";
            return info;
        }
        if (*method != "subscription") {
            return info; // Unknown: ignored, never fatal
        }
        const auto channel = wire::req_str(*params, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        const auto parts = split_channel(*channel);
        info.kind = kind_from_channel(parts);
        if (parts.count >= 2 && names_instrument(info.kind)) {
            info.instrument = parts.parts[1];
        }
        return info;
    });
}

std::expected<WireResponse, WireError> parse_response(std::string_view frame) {
    return guarded([&]() -> std::expected<WireResponse, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireResponse out{};
        const auto id = wire::opt_i64(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = *id;

        wire::ondemand::object error;
        const auto error_ec = wire::find(*root, "error").get_object().get(error);
        if (error_ec == simdjson::SUCCESS) {
            const auto code = wire::opt_i64(error, "code");
            const auto message = wire::opt_str(error, "message");
            if (!code.has_value() || !message.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.error_code = *code;
            out.error_message = *message;
            return out;
        }
        if (error_ec != simdjson::NO_SUCH_FIELD && error_ec != simdjson::INCORRECT_TYPE) {
            return std::unexpected(wire::map_error(error_ec));
        }

        wire::ondemand::array accepted;
        if (wire::find(*root, "result").get_array().get(accepted) == simdjson::SUCCESS) {
            for (auto entry : accepted) {
                if (const auto ec = entry.error(); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                ++out.accepted;
            }
        }
        return out;
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto token = req_num(*root, "result");
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        const auto ms = token_to_u64(*token);
        if (ms == 0) {
            return std::unexpected(WireError::WrongType);
        }
        return ms;
    });
}

} // namespace raikiri::venues::deribit
