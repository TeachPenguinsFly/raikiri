// dydx: the indexer's GET /v4/candles/perpetualMarkets/{ticker}, newest first, every minute present even without
// trades; baseTokenVolume is in the base asset, which is what our dydx prints count in
#include <algorithm>
#include <chrono>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t number(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

std::string iso(std::int64_t seconds) {
    const std::chrono::sys_seconds at{std::chrono::seconds{seconds}};
    const auto day = std::chrono::floor<std::chrono::days>(at);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss hms{at - day};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
                       static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()),
                       hms.hours().count(),
                       hms.minutes().count(),
                       hms.seconds().count());
}

// "2026-11-15T17:56:00.000Z"
std::optional<std::int32_t> minute_of_iso(std::string_view text) noexcept {
    if (text.size() < 16 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':') {
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

class Dydx final : public KlineVenue {
public:
    // toISO a second short of `to`, so the bar at `to` stays out whether the bound is inclusive or not
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "indexer.dydx.trade",
                .target = std::format("/v4/candles/perpetualMarkets/{}?resolution=1MIN&fromISO={}&toISO={}&limit={}",
                                      symbol,
                                      iso(static_cast<std::int64_t>(from) * 60),
                                      iso(static_cast<std::int64_t>(to) * 60 - 1),
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // 60 of the 600 a minute the ip has
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["candles"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto item : rows) {
            simdjson::dom::object row;
            std::string_view started;
            if (item.get(row) != simdjson::SUCCESS || row["startedAt"].get(started) != simdjson::SUCCESS) {
                continue;
            }
            const auto minute = minute_of_iso(started);
            std::int64_t trades = 0;
            if (!minute.has_value()) {
                continue;
            }
            if (row["trades"].get(trades) != simdjson::SUCCESS) {
                trades = 0;
            }
            out.push_back({.instrument = instrument,
                           .minute = *minute,
                           .open = number(row, "open"),
                           .high = number(row, "high"),
                           .low = number(row, "low"),
                           .close = number(row, "close"),
                           .volume = number(row, "baseTokenVolume"),
                           .trades = static_cast<std::int32_t>(trades),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> dydx(MarketType market) {
    // dydx v4 lists perps only
    if (market != MarketType::Perp) {
        return nullptr;
    }
    return std::make_unique<Dydx>();
}

} // namespace raikiri::candles::venues
