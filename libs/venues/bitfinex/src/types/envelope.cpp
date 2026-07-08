#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::bitfinex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

using JsonType = detail::ondemand::json_type;

constexpr std::string_view k_error_tag = "error";

// Matched against the raw body, quotes included, so the sniff below never has
// to index into a buffer whose length it has not already checked.
constexpr std::string_view k_quoted_error_tag = R"("error")";

FrameKind kind_from_event(std::string_view event) noexcept {
    if (event == "info") {
        return FrameKind::Info;
    }
    if (event == "subscribed") {
        return FrameKind::Subscribed;
    }
    if (event == "unsubscribed") {
        return FrameKind::Unsubscribed;
    }
    if (event == "error") {
        return FrameKind::Error;
    }
    if (event == "pong") {
        return FrameKind::Pong;
    }
    if (event == "auth") {
        return FrameKind::Auth;
    }
    return FrameKind::Unknown;
}

} // namespace

bool is_array_frame(std::string_view frame) noexcept {
    const auto text = detail::trim(frame);
    return !text.empty() && text.front() == '[';
}

// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so quotes come off after trimming. Contents stay escaped: Bitfinex symbols
// and decimal tokens cannot contain an escape, and letting simdjson unescape
// would move every field into a buffer that cannot be rebased.
std::expected<std::string_view, WireError> token_of(detail::ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    raw = detail::trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return session().rebase(raw.substr(1, raw.size() - 2));
    }
    return session().rebase(raw);
}

bool token_to_i64(std::string_view s, std::int64_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto* const end = s.data() + s.size();
    const auto [stop, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && stop == end;
}

std::expected<std::string_view, WireError> opt_token(detail::ondemand::object& obj,
                                                     std::string_view key) {
    detail::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return token_of(value);
}

std::expected<FrameInfo, WireError> parse_frame(std::string_view frame) {
    return guarded([&]() -> std::expected<FrameInfo, WireError> {
        FrameInfo info{};
        if (!is_array_frame(frame)) {
            auto root = session().open_object(frame);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            const auto event = opt_token(*root, "event");
            const auto code = detail::opt_i64(*root, "code");
            if (!event.has_value() || !code.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            info.kind = kind_from_event(*event);
            info.code = *code;
            return info;
        }

        auto root = session().open_array(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t index = 0;
        for (auto entry : *root) {
            detail::ondemand::value value;
            if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            if (index == 0) {
                const auto token = token_of(value);
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                if (!token_to_i64(*token, info.chan_id)) {
                    return std::unexpected(WireError::WrongType);
                }
                ++index;
                continue;
            }
            JsonType type{};
            if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            if (index == 1 && type == JsonType::string) {
                const auto token = token_of(value);
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                info.tag = *token;
            } else if (type == JsonType::number) {
                const auto token = token_of(value);
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                info.ts_ms = detail::digits_to_u64(*token);
            }
            ++index;
        }
        if (index < 2) {
            return std::unexpected(WireError::ShortArray);
        }
        info.kind = info.tag == "hb" ? FrameKind::Heartbeat : FrameKind::Data;
        return info;
    });
}

std::expected<WireSubscribed, WireError> parse_subscribed(std::string_view frame) {
    return guarded([&]() -> std::expected<WireSubscribed, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto chan_id = detail::opt_i64(*root, "chanId");
        const auto channel = opt_token(*root, "channel");
        const auto symbol = opt_token(*root, "symbol");
        const auto key = opt_token(*root, "key");
        if (!chan_id.has_value() || !channel.has_value() || !symbol.has_value() ||
            !key.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (*chan_id == 0 || channel->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireSubscribed{
            .chan_id = *chan_id, .channel = *channel, .symbol = *symbol, .key = *key};
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::opt_i64(*root, "code");
        const auto msg = opt_token(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .msg = *msg};
    });
}

std::expected<WireAuthAck, WireError> parse_auth_ack(std::string_view frame) {
    return guarded([&]() -> std::expected<WireAuthAck, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto status = opt_token(*root, "status");
        const auto msg = opt_token(*root, "msg");
        const auto code = detail::opt_i64(*root, "code");
        if (!status.has_value() || !msg.has_value() || !code.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return WireAuthAck{.ok = *status == "OK", .code = *code, .msg = *msg};
    });
}

bool is_rest_error(std::string_view body) noexcept {
    auto text = detail::trim(body);
    if (text.empty() || text.front() != '[') {
        return false;
    }
    text.remove_prefix(1);
    return detail::trim(text).starts_with(k_quoted_error_tag);
}

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto row = read_row<k_rest_error_arity>(*root);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        if (row->at<k_rest_error_tag>() != k_error_tag) {
            return std::unexpected(WireError::WrongType);
        }
        std::int64_t code = 0;
        if (!token_to_i64(row->at<k_rest_error_code>(), code)) {
            return std::unexpected(WireError::WrongType);
        }
        return RestError{.code = code, .msg = row->at<k_rest_error_msg>()};
    });
}

std::expected<std::int64_t, WireError> parse_platform_status(std::string_view body) {
    return guarded([&]() -> std::expected<std::int64_t, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto row = read_row<1>(*root);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        std::int64_t status = 0;
        if (!token_to_i64(row->at<0>(), status)) {
            return std::unexpected(WireError::WrongType);
        }
        return status;
    });
}

} // namespace raikiri::venues::bitfinex
