// bitfinex: GET /v2/candles/trade:1m:{symbol}/hist, rows [mts, open, close, high, low, volume]; the volume is in the
// base asset, as is the amount of our bitfinex prints, perps (tBTCF0:USTF0) included
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Bitfinex final : public KlineVenue {
public:
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api-pub.bitfinex.com",
                .target = std::format("/v2/candles/trade:1m:{}/hist?start={}&end={}&limit={}&sort=1",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1,
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return 10'000; }

    // the gateway already keeps 27 calls a minute for all of bitfinex's rest
    std::uint32_t per_minute() const noexcept override { return 6; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS || !root.is_array()) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        const auto rows = root.get_array().value_unsafe();
        // a refusal is an array too: ["error", code, text]
        if (rows.size() > 0 && rows.at(0).value_unsafe().is_string()) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            if (!row.is_array() || row.get_array().value_unsafe().size() < 6) {
                continue;
            }
            const auto a = row.get_array().value_unsafe();
            const auto mts = a.at(0).get_int64();
            if (mts.error() != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(mts.value_unsafe() / 60'000),
                           .open = e9(a.at(1).value_unsafe()),
                           .high = e9(a.at(3).value_unsafe()),
                           .low = e9(a.at(4).value_unsafe()),
                           .close = e9(a.at(2).value_unsafe()),
                           .volume = e9(a.at(5).value_unsafe()),
                           .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> bitfinex(MarketType market) {
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr; // our bitfinex catalogue lists no dated futures or options
    }
    return std::make_unique<Bitfinex>();
}

} // namespace raikiri::candles::venues
