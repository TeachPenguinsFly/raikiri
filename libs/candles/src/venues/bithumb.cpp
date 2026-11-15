// bithumb: upbit's v1 api, GET /v1/candles/minutes/1, newest first [{candle_date_time_utc, opening_price, high_price,
// low_price, trade_price, candle_acc_trade_volume}]; the volume is in the base coin, as is the trade_volume of our
// bithumb prints
#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

// bithumb reads `to` in korean time and refuses a zone suffix
constexpr std::int64_t k_kst_minutes = 9LL * 60;

// unix minutes as "yyyy-mm-ddThh:mm:00"
std::string clock_text(std::int64_t minute) {
    const auto days = std::chrono::floor<std::chrono::days>(std::chrono::minutes{minute});
    const std::chrono::year_month_day date{std::chrono::sys_days{days}};
    const auto rest = minute - std::chrono::minutes{days}.count();
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:00",
                       static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()),
                       rest / 60,
                       rest % 60);
}

// "yyyy-mm-ddThh:mm:ss" as unix minutes
std::optional<std::int32_t> minute_of_text(std::string_view text) noexcept {
    if (text.size() < 16) {
        return std::nullopt;
    }
    const auto number = [text](std::size_t at, std::size_t width) {
        int value = -1;
        const auto* const end = text.data() + at + width;
        const auto [stop, ec] = std::from_chars(text.data() + at, end, value);
        return ec == std::errc{} && stop == end ? value : -1;
    };
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    const int hour = number(11, 2);
    const int minute = number(14, 2);
    if (year < 1970 || month < 0 || day < 0 || hour < 0 || minute < 0) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch().count();
    return static_cast<std::int32_t>(days * 1440 + hour * 60 + minute);
}

class Bithumb final : public KlineVenue {
public:
    // `to` leaves out its own minute; a minute without trades has no row, so the count may reach before `from`
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.bithumb.com",
                .target = std::format("/v1/candles/minutes/1?market={}&to={}&count={}",
                                      symbol,
                                      clock_text(static_cast<std::int64_t>(to) + k_kst_minutes),
                                      to - from)};
    }

    std::int32_t per_call() const noexcept override { return 200; }

    // the gateway keeps 20 a second for itself; a backfill stays a trickle beside it
    std::uint32_t per_minute() const noexcept override { return 30; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS || !root.is_array()) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        const auto row_list = root.get_array().value_unsafe();
        for (const auto row : row_list) {
            std::string_view stamp;
            simdjson::dom::element o;
            simdjson::dom::element h;
            simdjson::dom::element l;
            simdjson::dom::element c;
            simdjson::dom::element v;
            if (row["candle_date_time_utc"].get(stamp) != simdjson::SUCCESS ||
                row["opening_price"].get(o) != simdjson::SUCCESS || row["high_price"].get(h) != simdjson::SUCCESS ||
                row["low_price"].get(l) != simdjson::SUCCESS || row["trade_price"].get(c) != simdjson::SUCCESS ||
                row["candle_acc_trade_volume"].get(v) != simdjson::SUCCESS) {
                continue;
            }
            const auto minute = minute_of_text(stamp);
            if (!minute.has_value()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = *minute,
                           .open = e9(o),
                           .high = e9(h),
                           .low = e9(l),
                           .close = e9(c),
                           .volume = e9(v),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto [first, last] = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(first, last);
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> bithumb(MarketType market) {
    if (market != MarketType::Spot) {
        return nullptr; // bithumb is a spot exchange
    }
    return std::make_unique<Bithumb>();
}

} // namespace raikiri::candles::venues
