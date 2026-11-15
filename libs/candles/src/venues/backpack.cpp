// backpack: GET /api/v1/klines, [{start "yyyy-mm-dd hh:mm:ss" utc, open, high, low, close, volume, trades}]; the volume
// is in the base asset, as is the q of our backpack prints
#include <charconv>
#include <chrono>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

// "yyyy-mm-dd hh:mm:ss" as unix minutes
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

class Backpack final : public KlineVenue {
public:
    // seconds; endTime leaves out its own bucket
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.backpack.exchange",
                .target = std::format("/api/v1/klines?symbol={}&interval=1m&startTime={}&endTime={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to) * 60)};
    }

    // a longer range is refused
    std::int32_t per_call() const noexcept override { return 1000; }

    // the config keeps 1800 a minute for the ip
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS || !root.is_array()) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : root.get_array().value_unsafe()) {
            std::string_view start;
            simdjson::dom::element o;
            simdjson::dom::element h;
            simdjson::dom::element l;
            simdjson::dom::element c;
            simdjson::dom::element v;
            if (row["start"].get(start) != simdjson::SUCCESS || row["open"].get(o) != simdjson::SUCCESS ||
                row["high"].get(h) != simdjson::SUCCESS || row["low"].get(l) != simdjson::SUCCESS ||
                row["close"].get(c) != simdjson::SUCCESS || row["volume"].get(v) != simdjson::SUCCESS) {
                continue;
            }
            const auto minute = minute_of_text(start);
            if (!minute.has_value()) {
                continue;
            }
            std::int32_t trades = 0;
            if (std::string_view text; row["trades"].get(text) == simdjson::SUCCESS) {
                const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), trades);
                if (ec != std::errc{} || end != text.data() + text.size()) {
                    trades = 0;
                }
            }
            out.push_back({.instrument = instrument,
                           .minute = *minute,
                           .open = e9(o),
                           .high = e9(h),
                           .low = e9(l),
                           .close = e9(c),
                           .volume = e9(v),
                           .trades = trades,
                           .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> backpack(MarketType market) {
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr; // backpack lists no dated futures or options
    }
    return std::make_unique<Backpack>();
}

} // namespace raikiri::candles::venues
