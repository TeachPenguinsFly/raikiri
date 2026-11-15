// weex: GET /capi/v3/market/historyKlines and /api/v3/market/historyKlines, rows [open ms, "o", "h", "l", "c",
// "volume", close ms, "quote", trades, ...] newest first; the volume is in the base asset, as the prints' q our feed
// folds on both markets. /market/klines would ignore the range and answer the newest bars
#include <algorithm>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Weex final : public KlineVenue {
public:
    explicit Weex(bool perp) : perp_{perp} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = perp_ ? "api-contract.weex.com" : "api-spot.weex.com",
                .target = std::format("{}?symbol={}&interval=1m&startTime={}&endTime={}&limit={}",
                                      perp_ ? "/capi/v3/market/historyKlines" : "/api/v3/market/historyKlines",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1,
                                      per_call())};
    }

    // a larger limit is refused
    std::int32_t per_call() const noexcept override { return 100; }

    // each market has 450 weight each 10 s an ip; 60 calls a minute stays under a quarter of it up to weight 10
    std::uint32_t per_minute() const noexcept override { return 60; }

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
            if (!row.is_array() || row.get_array().value_unsafe().size() < 9) {
                continue;
            }
            const auto a = row.get_array().value_unsafe();
            const auto open_ms = a.at(0).get_int64();
            const auto trades = a.at(8).get_int64();
            if (open_ms.error() != simdjson::SUCCESS) {
                continue;
            }
            out.push_back(
                {.instrument = instrument,
                 .minute = static_cast<std::int32_t>(open_ms.value_unsafe() / 60'000),
                 .open = e9(a.at(1).value_unsafe()),
                 .high = e9(a.at(2).value_unsafe()),
                 .low = e9(a.at(3).value_unsafe()),
                 .close = e9(a.at(4).value_unsafe()),
                 .volume = e9(a.at(5).value_unsafe()),
                 .trades = trades.error() == simdjson::SUCCESS ? static_cast<std::int32_t>(trades.value_unsafe()) : 0,
                 .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto repeated = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(repeated.begin(), repeated.end());
        return out;
    }

private:
    bool perp_;
};

} // namespace

std::unique_ptr<KlineVenue> weex(MarketType market) {
    // the catalogue lists perps and spot only
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Weex>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
