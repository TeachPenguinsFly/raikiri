// coinbase: GET /products/{id}/candles on the exchange api, rows [time s, low, high, open, close, volume] newest
// first; the volume is in the base asset, as is the size of our coinbase prints
#include <algorithm>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Coinbase final : public KlineVenue {
public:
    // both start and end include their bucket
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.exchange.coinbase.com",
                .target = std::format("/products/{}/candles?granularity=60&start={}&end={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to - 1) * 60)};
    }

    std::int32_t per_call() const noexcept override { return 300; }

    // the public api allows an ip 10 a second
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
            if (!row.is_array() || row.get_array().value_unsafe().size() < 6) {
                continue;
            }
            const auto a = row.get_array().value_unsafe();
            const auto time = a.at(0).get_int64();
            if (time.error() != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(time.value_unsafe() / 60),
                           .open = e9(a.at(3).value_unsafe()),
                           .high = e9(a.at(2).value_unsafe()),
                           .low = e9(a.at(1).value_unsafe()),
                           .close = e9(a.at(4).value_unsafe()),
                           .volume = e9(a.at(5).value_unsafe()),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto [first, last] = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(first, last);
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> coinbase(MarketType market) {
    // our coinbase catalogue is the spot exchange; its perps live on the separate international api
    if (market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Coinbase>();
}

} // namespace raikiri::candles::venues
