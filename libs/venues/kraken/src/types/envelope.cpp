#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::kraken {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

[[nodiscard]] constexpr bool is_number_char(char c) noexcept {
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E';
}

EventKind kind_from_feed(std::string_view feed) noexcept {
    if (feed == "ticker") {
        return EventKind::Ticker;
    }
    if (feed == "ticker_lite") {
        return EventKind::TickerLite;
    }
    if (feed == "trade" || feed == "trade_snapshot") {
        return EventKind::Trade;
    }
    if (feed == "book" || feed == "book_snapshot") {
        return EventKind::Book;
    }
    if (feed == "open_orders_snapshot") {
        return EventKind::OpenOrdersSnapshot;
    }
    if (feed == "open_orders") {
        return EventKind::OpenOrders;
    }
    // Snapshot and delta are one shape here, unlike open_orders: both carry a
    // `fills` array under the same key.
    if (feed == "fills" || feed == "fills_snapshot") {
        return EventKind::Fills;
    }
    return EventKind::Unknown;
}

EventKind kind_from_event(std::string_view event) noexcept {
    if (event == "subscribed" || event == "unsubscribed") {
        return EventKind::Subscribed;
    }
    if (event == "info") {
        return EventKind::Info;
    }
    if (event == "alert" || event == "error") {
        return EventKind::Alert;
    }
    if (event == "challenge") {
        return EventKind::Challenge;
    }
    return EventKind::Unknown;
}

// Days since 1970-01-01 from a proleptic Gregorian date (Hinnant). Integer
// only: this feeds a clock offset, and a double loses whole seconds past
// 2^53 nanoseconds.
[[nodiscard]] constexpr std::int64_t
days_from_civil(std::int64_t y, std::uint32_t m, std::uint32_t d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<std::uint32_t>(y - era * 400);
    const std::uint32_t doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;
    const std::uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Fixed-width unsigned field at `offset`. Returns false on anything that is
// not exactly `width` digits, so a reshaped timestamp fails loudly.
[[nodiscard]] bool field_at(std::string_view text,
                            std::size_t offset,
                            std::size_t width,
                            std::uint32_t& out) noexcept {
    if (offset + width > text.size()) {
        return false;
    }
    const auto* const begin = text.data() + offset;
    const auto [stop, ec] = std::from_chars(begin, begin + width, out);
    return ec == std::errc{} && stop == begin + width;
}

} // namespace

std::expected<std::string_view, WireError> num_of(detail::ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    raw = detail::trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    for (const char c : raw) {
        if (!is_number_char(c)) {
            return std::unexpected(WireError::WrongType);
        }
    }
    if (raw.empty()) {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw);
}

std::expected<std::string_view, WireError> opt_num(detail::ondemand::object& obj,
                                                   std::string_view key) {
    detail::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return num_of(value);
}

std::expected<std::string_view, WireError> req_num(detail::ondemand::object& obj,
                                                   std::string_view key) {
    detail::ondemand::value value;
    if (const auto ec = wire::find(obj, key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    auto text = num_of(value);
    if (text && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::expected<std::int64_t, WireError>
opt_i64(detail::ondemand::object& obj, std::string_view key, bool& present) {
    present = false;
    std::int64_t out = 0;
    const auto ec = wire::find(obj, key).get_int64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::int64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    present = true;
    return out;
}

std::expected<std::uint64_t, WireError> opt_ms(detail::ondemand::object& obj,
                                               std::string_view key) {
    const auto text = opt_num(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return detail::digits_to_u64(*text);
}

std::expected<std::uint64_t, WireError> parse_iso8601_ms(std::string_view text) {
    constexpr std::size_t k_seconds_len = 19;
    if (text.size() < k_seconds_len) {
        return std::unexpected(WireError::WrongType);
    }
    std::uint32_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;
    std::uint32_t hour = 0;
    std::uint32_t minute = 0;
    std::uint32_t second = 0;
    if (!field_at(text, 0, 4, year) || !field_at(text, 5, 2, month) || !field_at(text, 8, 2, day) ||
        !field_at(text, 11, 2, hour) || !field_at(text, 14, 2, minute) ||
        !field_at(text, 17, 2, second)) {
        return std::unexpected(WireError::WrongType);
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::unexpected(WireError::WrongType);
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return std::unexpected(WireError::WrongType);
    }

    std::uint64_t millis = 0;
    if (text.size() > k_seconds_len && text[k_seconds_len] == '.') {
        std::uint32_t scale = 100;
        for (std::size_t i = k_seconds_len + 1; i < text.size(); ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                break;
            }
            if (scale == 0) {
                continue; // digits below millisecond resolution are dropped
            }
            millis += static_cast<std::uint64_t>(c - '0') * scale;
            scale /= 10;
        }
    }

    const auto days = days_from_civil(year, month, day);
    const auto seconds = days * 86400 + hour * 3600LL + minute * 60LL + second;
    if (seconds < 0) {
        return std::unexpected(WireError::WrongType);
    }
    return static_cast<std::uint64_t>(seconds) * 1000ULL + millis;
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto event = detail::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        EventInfo info{};
        if (!event->empty()) {
            info.kind = kind_from_event(*event);
            return info; // control frames name product ids in an array, not a field
        }
        const auto feed = detail::req_str(*root, "feed");
        if (!feed.has_value()) {
            return std::unexpected(feed.error());
        }
        info.kind = kind_from_feed(*feed);
        const auto product = detail::opt_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        info.product_id = *product;
        return info;
    });
}

std::expected<StreamAlert, WireError> parse_stream_alert(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamAlert, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto message = detail::req_str(*root, "message");
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        return StreamAlert{.message = *message};
    });
}

std::expected<WireChallenge, WireError> parse_challenge(std::string_view frame) {
    return guarded([&]() -> std::expected<WireChallenge, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto message = detail::req_str(*root, "message");
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        return WireChallenge{.message = *message};
    });
}

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto result = detail::req_str(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        RestEnvelope out{};
        out.ok = *result == "success";
        if (!out.ok) {
            const auto error = detail::opt_str(*root, "error");
            if (!error.has_value()) {
                return std::unexpected(error.error());
            }
            out.error = *error;
            if (out.error.empty()) {
                const auto status = detail::opt_str(*root, "status");
                if (!status.has_value()) {
                    return std::unexpected(status.error());
                }
                out.error = *status;
            }
        }
        const auto server_time = detail::opt_str(*root, "serverTime");
        if (!server_time.has_value()) {
            return std::unexpected(server_time.error());
        }
        if (!server_time->empty()) {
            if (const auto ms = parse_iso8601_ms(*server_time); ms.has_value()) {
                out.server_ms = *ms;
            }
        }
        return out;
    });
}

} // namespace raikiri::venues::kraken
