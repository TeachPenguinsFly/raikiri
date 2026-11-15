// bitunix: GET /api/v1/futures/market/kline and the spot /api/spot/v1/market/kline/history, both newest first; the
// futures baseVol and the spot volume are in the base coin, which is what our bitunix prints count in
#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t number(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

// the futures open time is a string of ms
std::optional<std::int32_t> minute_of_ms(simdjson::dom::object row) noexcept {
    std::string_view text;
    std::int64_t ms = 0;
    if (row["time"].get(text) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), ms);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(ms / 60'000);
}

// the spot open time is "2026-11-15T17:56:00Z"
std::optional<std::int32_t> minute_of_iso(simdjson::dom::object row) noexcept {
    std::string_view text;
    if (row["ts"].get(text) != simdjson::SUCCESS || text.size() < 16 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':') {
        return std::nullopt;
    }
    const auto digits = [text](std::size_t at, std::size_t len) {
        int out = 0;
        for (std::size_t i = at; i < at + len; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return -1;
            }
            out = out * 10 + (text[i] - '0');
        }
        return out;
    };
    const int year = digits(0, 4);
    const int hour = digits(11, 2);
    const int minute = digits(14, 2);
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(digits(5, 2))},
                                           std::chrono::day{static_cast<unsigned>(digits(8, 2))}};
    if (year < 0 || !date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch().count();
    return static_cast<std::int32_t>(days * 1440 + hour * 60 + minute);
}

class Bitunix final : public KlineVenue {
public:
    explicit Bitunix(bool perp) : perp_{perp} {}

    // futures take [startTime, endTime) in ms; spot has no start bound and pages back from endTime in seconds, a quiet
    // minute kept flat, so the limit stops it at `from`
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        if (perp_) {
            return {.host = "fapi.bitunix.com",
                    .target = std::format("/api/v1/futures/market/kline?symbol={}&interval=1m&startTime={}&endTime={}"
                                          "&limit={}",
                                          symbol,
                                          static_cast<std::int64_t>(from) * 60'000,
                                          static_cast<std::int64_t>(to) * 60'000,
                                          per_call())};
        }
        return {.host = "openapi.bitunix.com",
                .target = std::format("/api/spot/v1/market/kline/history?symbol={}&interval=1&endTime={}&limit={}",
                                      symbol,
                                      static_cast<std::int64_t>(to) * 60,
                                      to - from)};
    }

    // futures cut a bigger limit to 200; spot refuses 1000
    std::int32_t per_call() const noexcept override { return perp_ ? 200 : 500; }

    // 30 for each market of the 600 a minute the ip has for market data
    std::uint32_t per_minute() const noexcept override { return 30; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto item : rows) {
            simdjson::dom::object row;
            if (item.get(row) != simdjson::SUCCESS) {
                continue;
            }
            const auto minute = perp_ ? minute_of_ms(row) : minute_of_iso(row);
            if (!minute.has_value()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = *minute,
                           .open = number(row, "open"),
                           .high = number(row, "high"),
                           .low = number(row, "low"),
                           .close = number(row, "close"),
                           .volume = number(row, perp_ ? "baseVol" : "volume"),
                           .trades = 0,
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        return out;
    }

private:
    bool perp_;
};

} // namespace

std::unique_ptr<KlineVenue> bitunix(MarketType market) {
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Bitunix>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
