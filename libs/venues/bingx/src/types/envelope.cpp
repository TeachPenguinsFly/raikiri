#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bingx {

namespace {

using wire::guarded;
using wire::session;

// exactly these bytes after inflation; answered with "Pong"
constexpr std::string_view k_ping = "Ping";

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "bookTicker") {
        return EventKind::BookTicker;
    }
    if (channel == "trade") {
        return EventKind::Trade;
    }
    if (channel == "markPrice") {
        return EventKind::MarkPrice;
    }
    if (channel == "ticker") {
        return EventKind::Ticker;
    }
    if (channel.starts_with("depth")) {
        return EventKind::Depth;
    }
    return EventKind::Unknown;
}

} // namespace

std::expected<std::int64_t, WireError> req_i64(wire::ondemand::object& obj,
                                               std::string_view key) {
    std::int64_t out = 0;
    if (const auto ec = wire::find(obj, key).get_int64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return out;
}

std::string_view symbol_of(std::string_view data_type) {
    const auto at = data_type.find('@');
    return at == std::string_view::npos ? std::string_view{} : data_type.substr(0, at);
}

// quotes come off after trimming; contents stay escaped so views stay in the caller's buffer
std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value) {
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
    }
    return session().rebase(raw);
}

std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj,
                                                      std::string_view key) {
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

std::expected<std::string_view, WireError> req_scalar(wire::ondemand::object& obj,
                                                      std::string_view key) {
    const auto text = opt_scalar(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::expected<std::uint64_t, WireError> opt_ms_any(wire::ondemand::object& obj,
                                                   std::string_view key) {
    const auto text = opt_scalar(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return wire::digits_to_u64(*text);
}

bool is_ping(std::string_view frame) noexcept {
    return wire::trim(frame) == k_ping;
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    if (is_ping(frame)) {
        return EventInfo{.kind = EventKind::Ping, .symbol = {}};
    }
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto data_type = wire::opt_str(*root, "dataType");
        if (!data_type.has_value()) {
            return std::unexpected(data_type.error());
        }
        EventInfo info{};
        if (data_type->empty()) {
            const auto code = req_i64(*root, "code");
            if (!code.has_value()) {
                return std::unexpected(code.error());
            }
            info.kind = *code == k_ok ? EventKind::ControlAck : EventKind::Error;
            return info;
        }
        info.symbol = symbol_of(*data_type);
        info.kind = kind_from_channel(data_type->substr(info.symbol.size() + 1));
        return info;
    });
}

std::expected<ControlFrame, WireError> parse_control(std::string_view frame) {
    return guarded([&]() -> std::expected<ControlFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto id = wire::opt_str(*root, "id");
        const auto code = wire::opt_i64(*root, "code");
        const auto msg = wire::opt_str(*root, "msg");
        if (!id.has_value() || !code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return ControlFrame{.id = *id, .code = *code, .msg = *msg};
    });
}

std::expected<PrivateKind, WireError> parse_private_event(std::string_view frame) {
    return guarded([&]() -> std::expected<PrivateKind, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto event = wire::opt_str(*root, "e");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        if (*event == "ORDER_TRADE_UPDATE") {
            return PrivateKind::OrderUpdate;
        }
        if (*event == "listenKeyExpired") {
            return PrivateKind::ListenKeyExpired;
        }
        if (*event == "ACCOUNT_UPDATE" || *event == "ACCOUNT_CONFIG_UPDATE") {
            // the sink has no balance or leverage channel; positions are reconciled instead
            return PrivateKind::Ignorable;
        }
        const auto data_type = wire::opt_str(*root, "dataType");
        if (!data_type.has_value()) {
            return std::unexpected(data_type.error());
        }
        if (*data_type == "spot.executionReport") {
            return PrivateKind::OrderUpdate;
        }
        if (*data_type == "ACCOUNT_UPDATE") {
            return PrivateKind::Ignorable;
        }
        if (data_type->empty()) {
            // the code alone separates an ack from a refusal
            const auto code = wire::opt_i64(*root, "code");
            if (!code.has_value()) {
                return std::unexpected(code.error());
            }
            return *code == k_ok ? PrivateKind::Ack : PrivateKind::Error;
        }
        return PrivateKind::Unknown;
    });
}

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = req_i64(*root, "code");
        const auto msg = wire::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return RestEnvelope{.code = *code, .msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto ts = wire::opt_u64(*data, "serverTime");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::bingx
