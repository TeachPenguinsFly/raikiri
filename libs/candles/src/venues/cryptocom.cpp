// crypto.com: GET /exchange/v1/public/get-candlestick, result.data [{t ms, o, h, l, c, v}]; v is in the base asset for
// pairs and perps alike, the unit of the q our crypto.com prints carry
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class CryptoCom final : public KlineVenue {
public:
    // end_ts leaves out its own bucket
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {
            .host = "api.crypto.com",
            .target = std::format(
                "/exchange/v1/public/get-candlestick?instrument_name={}&timeframe=1m&count={}&start_ts={}&end_ts={}",
                symbol,
                per_call(),
                static_cast<std::int64_t>(from) * 60'000,
                static_cast<std::int64_t>(to) * 60'000)};
    }

    std::int32_t per_call() const noexcept override { return 300; }

    // market data allows an ip 100 calls a second
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::int64_t code = -1;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get(code) != simdjson::SUCCESS || code != 0 ||
            root["result"]["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            std::int64_t t = 0;
            simdjson::dom::element o;
            simdjson::dom::element h;
            simdjson::dom::element l;
            simdjson::dom::element c;
            simdjson::dom::element v;
            if (row["t"].get(t) != simdjson::SUCCESS || row["o"].get(o) != simdjson::SUCCESS ||
                row["h"].get(h) != simdjson::SUCCESS || row["l"].get(l) != simdjson::SUCCESS ||
                row["c"].get(c) != simdjson::SUCCESS || row["v"].get(v) != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(t / 60'000),
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

std::unique_ptr<KlineVenue> cryptocom(MarketType market) {
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr; // our crypto.com catalogue lists no dated futures or options
    }
    return std::make_unique<CryptoCom>();
}

} // namespace raikiri::candles::venues
