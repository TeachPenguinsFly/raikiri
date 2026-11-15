// kraken: spot GET /0/public/OHLC, rows [time s, "o", "h", "l", "c", "vwap", "volume", count] with the last one still
// open; perps GET /api/charts/v1/trade/{symbol}/1m on futures.kraken.com, candles {time ms, open, high, low, close,
// volume}. spot volume is in the base asset like our spot prints, perp volume in contracts like a futures trade's qty
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class KrakenSpot final : public KlineVenue {
public:
    // there is no end: the answer runs to now and holds only the newest 720 minutes, so an older gap comes back empty
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t /*to*/) const override {
        return {.host = "api.kraken.com",
                .target = std::format(
                    "/0/public/OHLC?pair={}&interval=1&since={}", symbol, static_cast<std::int64_t>(from) * 60)};
    }

    std::int32_t per_call() const noexcept override { return 720; }

    // the public api refills about one call a second for an ip
    std::uint32_t per_minute() const noexcept override { return 12; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::object result;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS || !root.is_object() ||
            root["result"].get(result) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        simdjson::dom::array errors;
        if (root["error"].get(errors) == simdjson::SUCCESS && errors.size() > 0) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        // the newest committed row; anything after it is the open minute
        std::int64_t last = INT64_MAX;
        (void)result["last"].get(last);
        std::vector<Candle> out;
        for (const auto field : result) {
            if (!field.value.is_array()) {
                continue;
            }
            for (const auto row : field.value.get_array().value_unsafe()) {
                if (!row.is_array() || row.get_array().value_unsafe().size() < 8) {
                    continue;
                }
                const auto a = row.get_array().value_unsafe();
                const auto time = a.at(0).get_int64();
                const auto trades = a.at(7).get_int64();
                if (time.error() != simdjson::SUCCESS || time.value_unsafe() > last) {
                    continue;
                }
                out.push_back({.instrument = instrument,
                               .minute = static_cast<std::int32_t>(time.value_unsafe() / 60),
                               .open = e9(a.at(1).value_unsafe()),
                               .high = e9(a.at(2).value_unsafe()),
                               .low = e9(a.at(3).value_unsafe()),
                               .close = e9(a.at(4).value_unsafe()),
                               .volume = e9(a.at(6).value_unsafe()),
                               .trades = trades.error() == simdjson::SUCCESS
                                             ? static_cast<std::int32_t>(trades.value_unsafe())
                                             : 0,
                               .source = Source::Venue});
            }
        }
        return out;
    }
};

class KrakenFutures final : public KlineVenue {
public:
    // both from and to include their bucket
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "futures.kraken.com",
                .target = std::format("/api/charts/v1/trade/{}/1m?from={}&to={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to - 1) * 60)};
    }

    std::int32_t per_call() const noexcept override { return 2000; }

    std::uint32_t per_minute() const noexcept override { return 30; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array candles;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["candles"].get(candles) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : candles) {
            std::int64_t time = 0;
            simdjson::dom::element o;
            simdjson::dom::element h;
            simdjson::dom::element l;
            simdjson::dom::element c;
            simdjson::dom::element v;
            if (row["time"].get(time) != simdjson::SUCCESS || row["open"].get(o) != simdjson::SUCCESS ||
                row["high"].get(h) != simdjson::SUCCESS || row["low"].get(l) != simdjson::SUCCESS ||
                row["close"].get(c) != simdjson::SUCCESS || row["volume"].get(v) != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(time / 60'000),
                           .open = e9(o),
                           .high = e9(h),
                           .low = e9(l),
                           .close = e9(c),
                           .volume = e9(v),
                           .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> kraken(MarketType market) {
    if (market == MarketType::Spot) {
        return std::make_unique<KrakenSpot>();
    }
    if (market == MarketType::Perp) {
        return std::make_unique<KrakenFutures>();
    }
    return nullptr; // our kraken catalogue lists no dated futures or options
}

} // namespace raikiri::candles::venues
