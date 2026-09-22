#include <raikiri/venues/phemex/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::phemex {

namespace {

using wire::guarded;
using wire::session;

struct Named {
    std::string_view key;
    EventKind kind;
};

// A data frame is recognised by the key its payload sits under.
constexpr std::array<Named, 6> k_payloads{{
    {"orderbook_p", EventKind::RealBook},
    {"book", EventKind::ScaledBook},
    {"trades_p", EventKind::RealTrades},
    {"trades", EventKind::ScaledTrades},
    {"market24h", EventKind::InverseTicker},
    {"spot_market24h", EventKind::SpotTicker},
}};

} // namespace

std::expected<EventKind, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventKind, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        for (auto field : *root) {
            std::string_view key;
            if (const auto ec = field.unescaped_key().get(key); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            for (const auto& named : k_payloads) {
                if (key == named.key) {
                    return named.kind;
                }
            }
            if (key == "method") {
                const auto method = wire::str_of(field.value());
                if (method.has_value() && method->starts_with("perp_market24h_pack_p")) {
                    return EventKind::PerpPack;
                }
                return EventKind::Unknown;
            }
            if (key == "error") {
                bool is_null = false;
                if (const auto ec = field.value().is_null().get(is_null);
                    ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                return is_null ? EventKind::Reply : EventKind::Error;
            }
        }
        return EventKind::Unknown;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto error = wire::req_object(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        const auto code = wire::opt_i64(*error, "code");
        const auto message = wire::opt_str(*error, "message");
        if (!code.has_value() || !message.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        const auto id = wire::opt_u64(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        return StreamError{.code = *code, .message = *message, .id = *id};
    });
}

} // namespace raikiri::venues::phemex
