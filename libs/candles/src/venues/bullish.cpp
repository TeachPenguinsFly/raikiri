// bullish: GET /trading-api/v1/markets/{symbol}/candle, newest first and minutes without prints left out; volume is in
// the base asset, the unit of a print's quantity
#include <algorithm>
#include <charconv>
#include <chrono>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t e9_at(simdjson::dom::element row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

std::string iso(std::int64_t unix_s) {
    const std::chrono::sys_seconds at{std::chrono::seconds{unix_s}};
    const auto day = std::chrono::floor<std::chrono::days>(at);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{at - day};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.000Z",
                       static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()),
                       time.hours().count(),
                       time.minutes().count(),
                       time.seconds().count());
}

class Bullish final : public KlineVenue {
public:
    // both bounds are inclusive and the window may not be empty, so it ends on the last second of `to - 1`
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.exchange.bullish.com",
                .target = std::format("/trading-api/v1/markets/{}/candle?createdAtDatetime%5Bgte%5D={}"
                                      "&createdAtDatetime%5Blte%5D={}&timeBucket=1m&_pageSize={}",
                                      symbol,
                                      iso(from * 60LL),
                                      iso(to * 60LL - 1),
                                      per_call())};
    }

    // the largest page
    std::int32_t per_call() const noexcept override { return 100; }

    // the ip may make 50 calls a second
    std::uint32_t per_minute() const noexcept override { return 120; }

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
            std::int64_t open_ms{0};
            if (row["createdAtTimestamp"].get_string().get(stamp) != simdjson::SUCCESS ||
                std::from_chars(stamp.data(), stamp.data() + stamp.size(), open_ms).ec != std::errc{}) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_ms / 60'000),
                           .open = e9_at(row, "open"),
                           .high = e9_at(row, "high"),
                           .low = e9_at(row, "low"),
                           .close = e9_at(row, "close"),
                           .volume = e9_at(row, "volume"),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto repeated = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(repeated.begin(), repeated.end());
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> bullish(MarketType market) {
    // dated futures and options are not in our catalogue
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Bullish>();
}

} // namespace raikiri::candles::venues
