#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::xt {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_topic(std::string_view topic) noexcept {
    if (topic == "depth") {
        return EventKind::Depth;
    }
    if (topic == "depth_update") {
        return EventKind::DepthUpdate;
    }
    if (topic == "trade") {
        return EventKind::Trade;
    }
    if (topic == "mark_price") {
        return EventKind::MarkPrice;
    }
    if (topic == "ticker") {
        return EventKind::Ticker;
    }
    return EventKind::Unknown;
}

PrivateTopic topic_of(std::string_view topic) noexcept {
    if (topic == "order") {
        return PrivateTopic::Order;
    }
    if (topic == "trade") {
        return PrivateTopic::Trade;
    }
    if (topic == "position") {
        return PrivateTopic::Position;
    }
    if (topic == "balance") {
        return PrivateTopic::Balance;
    }
    return PrivateTopic::Unknown;
}

} // namespace

std::expected<std::int64_t, WireError> req_i64(detail::ondemand::object& obj,
                                               std::string_view key) {
    std::int64_t out = 0;
    if (const auto ec = wire::find(obj, key).get_int64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return out;
}

std::expected<bool, WireError> req_bool(detail::ondemand::object& obj, std::string_view key) {
    bool out = false;
    if (const auto ec = wire::find(obj, key).get_bool().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return out;
}

std::expected<std::uint64_t, WireError> opt_ms(detail::ondemand::object& obj,
                                               std::string_view key) {
    std::uint64_t out = 0;
    const auto ec = wire::find(obj, key).get_uint64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::uint64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return out;
}

std::expected<std::uint64_t, WireError> req_digits(detail::ondemand::object& obj,
                                                   std::string_view key) {
    const auto text = detail::req_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    const auto value = detail::digits_to_u64(*text);
    if (value == 0) {
        return std::unexpected(WireError::WrongType);
    }
    return value;
}

// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so quotes come off after trimming. Contents stay escaped: XT symbols, order
// ids and decimal tokens cannot contain an escape, and letting simdjson
// unescape would move every field into a buffer that cannot be rebased.
std::expected<std::string_view, WireError> scalar_of(detail::ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    raw = detail::trim(raw);
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

std::expected<std::string_view, WireError> opt_scalar(detail::ondemand::object& obj,
                                                      std::string_view key) {
    detail::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return scalar_of(value);
}

std::expected<std::int64_t, WireError>
opt_i64(detail::ondemand::object& obj, std::string_view key, std::int64_t fallback) {
    auto field = wire::find(obj, key);
    if (field.error() == simdjson::NO_SUCH_FIELD) {
        return fallback;
    }
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(field.error()));
    }
    // An explicit null is the venue saying "no value", which is what a fallback
    // is for. A wrong TYPE is not: XT spells some numbers as strings, and
    // folding that into the fallback would drop a value the venue did send.
    if (bool null = false; field.is_null().get(null) == simdjson::SUCCESS && null) {
        return fallback;
    }
    std::int64_t out = 0;
    if (const auto ec = field.get_int64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return out;
}

std::expected<bool, WireError>
opt_bool_or(detail::ondemand::object& obj, std::string_view key, bool fallback) {
    auto field = wire::find(obj, key);
    if (field.error() == simdjson::NO_SUCH_FIELD) {
        return fallback;
    }
    if (bool null = false; field.is_null().get(null) == simdjson::SUCCESS && null) {
        return fallback;
    }
    bool out = false;
    const auto ec = field.get_bool().get(out);
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return out;
}

// `result` is an ARRAY on the unpaged reads (/v4/open-order, position/list) and
// an OBJECT wrapping `items` on the paged ones (/v4/trade, order/trade-list).
// Asking for one shape and getting the other is not an error XT reports — the
// read simply returns nothing — so the shape is resolved before the walk.
std::expected<detail::ondemand::array, WireError> rows_of(detail::ondemand::object& root,
                                                          std::string_view key) {
    detail::ondemand::value result;
    if (const auto ec = wire::find(root, "result").get(result); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    detail::ondemand::json_type type{};
    if (const auto ec = result.type().get(type); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    if (type == detail::ondemand::json_type::array) {
        detail::ondemand::array array;
        if (const auto ec = result.get_array().get(array); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        return array;
    }
    if (type != detail::ondemand::json_type::object) {
        return std::unexpected(WireError::WrongType);
    }
    detail::ondemand::object page;
    if (const auto ec = result.get_object().get(page); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return detail::req_array(page, key);
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto topic = detail::opt_str(*root, "topic");
        if (!topic.has_value()) {
            return std::unexpected(topic.error());
        }
        EventInfo info{};
        if (topic->empty()) {
            const auto code = req_i64(*root, "code");
            info.kind = !code.has_value() ? EventKind::Unknown
                        : *code == k_ok   ? EventKind::Ack
                                          : EventKind::Error;
            return info;
        }
        info.kind = kind_from_topic(*topic);
        return with_data(
            *root, [&](detail::ondemand::object& data) -> std::expected<EventInfo, WireError> {
                const auto symbol = detail::req_str(data, "s");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                info.symbol = *symbol;
                return info;
            });
    });
}

std::expected<StreamAck, WireError> parse_stream_ack(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamAck, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = req_i64(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        const auto id = detail::opt_str(*root, "id");
        if (!code.has_value() || !msg.has_value() || !id.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamAck{.code = *code, .msg = *msg, .id = *id};
    });
}

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto return_code = req_i64(*root, "returnCode");
        const auto msg = detail::opt_str(*root, "msgInfo");
        if (!return_code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        RestEnvelope out{.return_code = *return_code, .code = {}, .msg = *msg};
        if (auto error = detail::req_object(*root, "error"); error.has_value()) {
            const auto code = detail::opt_str(*error, "code");
            const auto detail_msg = detail::opt_str(*error, "msg");
            if (!code.has_value() || !detail_msg.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.code = *code;
            if (!detail_msg->empty()) {
                out.msg = *detail_msg;
            }
        }
        return out;
    });
}

std::expected<SpotEnvelope, WireError> parse_spot_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<SpotEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto rc = req_i64(*root, "rc");
        const auto mc = opt_scalar(*root, "mc");
        if (!rc.has_value() || !mc.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return SpotEnvelope{.rc = *rc, .mc = *mc};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = opt_ms(*root, "result");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = detail::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto ms = opt_ms(*result, "serverTime");
        if (!ms.has_value()) {
            return std::unexpected(ms.error());
        }
        if (*ms == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ms;
    });
}

std::expected<std::string_view, WireError> parse_spot_ws_token(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = detail::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        const auto token = opt_scalar(*result, "accessToken");
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        if (token->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return *token;
    });
}

std::expected<std::string_view, WireError> parse_listen_key(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        detail::ondemand::value result;
        if (const auto ec = wire::find(*root, "result").get(result); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        detail::ondemand::json_type type{};
        if (const auto ec = result.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        // `result` is the key STRING itself. XT's own reference prints it as
        // `{}` — that is the doc's placeholder for every result, not a shape —
        // so an object is still read, for the one field it could hold.
        if (type != detail::ondemand::json_type::object) {
            const auto key = scalar_of(result);
            if (!key.has_value()) {
                return std::unexpected(key.error());
            }
            if (key->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            return *key;
        }
        detail::ondemand::object obj;
        if (const auto ec = result.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto key = opt_scalar(obj, "listenKey");
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
        if (key->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return *key;
    });
}

std::expected<PrivateFrame, WireError> parse_private_frame(std::string_view frame) {
    return guarded([&]() -> std::expected<PrivateFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (detail::has_field(*root, "topic")) {
            const auto topic = detail::req_str(*root, "topic");
            if (!topic.has_value()) {
                return std::unexpected(topic.error());
            }
            return PrivateFrame{.topic = topic_of(*topic), .code = 0, .msg = {}};
        }
        // The subscribe answer, identical in shape on both trees:
        // 0 accepted, 1 refused, 2 the listen key is no longer valid.
        const auto code = opt_i64(*root, "code", -1);
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        if (*code < 0) {
            return std::unexpected(WireError::UnknownMessage);
        }
        const auto msg = opt_scalar(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return PrivateFrame{.topic = *code == k_ok ? PrivateTopic::Ack : PrivateTopic::Refusal,
                            .code = *code,
                            .msg = *msg};
    });
}

} // namespace raikiri::venues::xt
