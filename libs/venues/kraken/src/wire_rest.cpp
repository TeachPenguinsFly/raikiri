#include <charconv>

#include "wire_detail.hpp"

namespace raikiri::venues::kraken {

namespace {

using detail::guarded;
using detail::session;

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

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "instruments");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireInstrument out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto type = detail::req_str(obj, "type");
            if (!symbol.has_value() || !type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.type = *type;
            const auto tick = detail::opt_num(obj, "tickSize");
            const auto contract = detail::opt_num(obj, "contractSize");
            const auto underlying = detail::opt_str(obj, "underlying");
            const auto last_trading = detail::opt_str(obj, "lastTradingTime");
            const auto tradeable = detail::opt_bool(obj, "tradeable");
            if (!tick.has_value() || !contract.has_value() || !underlying.has_value() ||
                !last_trading.has_value() || !tradeable.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.tick_size = *tick;
            out.contract_size = *contract;
            out.underlying = *underlying;
            out.last_trading_time = *last_trading;
            out.tradeable = *tradeable;
            const auto precision =
                detail::opt_i64(obj, "contractValueTradePrecision", out.has_precision);
            if (!precision.has_value()) {
                return std::unexpected(precision.error());
            }
            out.contract_value_trade_precision = *precision;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

} // namespace raikiri::venues::kraken
