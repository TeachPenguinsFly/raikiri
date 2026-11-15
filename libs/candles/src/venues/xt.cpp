// xt: perps GET /future/market/v1/public/q/kline, rows {"t": open ms, "o", "c", "h", "l", "a", "v"} with a in
// contracts as the futures prints; spot GET /v4/public/kline, rows {"t", "o", "c", "h", "l", "q", "v"} with q in the
// base asset as the spot prints; both newest first and neither states a trade count
#include <algorithm>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t field(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Xt final : public KlineVenue {
public:
    explicit Xt(bool perp) : perp_{perp} {}

    // a bar is in when it closes by endTime; perps ignore startTime and count limit bars back from endTime
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = perp_ ? "fapi.xt.com" : "sapi.xt.com",
                .target = std::format("{}?symbol={}&interval=1m&startTime={}&endTime={}&limit={}",
                                      perp_ ? "/future/market/v1/public/q/kline" : "/v4/public/kline",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000,
                                      to - from)};
    }

    std::int32_t per_call() const noexcept override { return perp_ ? 1500 : 1000; }

    // kept low: the public quotas are per ip and the gateway polls them too
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::int64_t code = -1;
        simdjson::dom::array result;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root[perp_ ? "returnCode" : "rc"].get(code) != simdjson::SUCCESS || code != 0 ||
            root["result"].get(result) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto entry : result) {
            simdjson::dom::object row;
            std::int64_t open_ms = 0;
            if (entry.get(row) != simdjson::SUCCESS || row["t"].get(open_ms) != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_ms / 60'000),
                           .open = field(row, "o"),
                           .high = field(row, "h"),
                           .low = field(row, "l"),
                           .close = field(row, "c"),
                           .volume = field(row, perp_ ? "a" : "q"),
                           .trades = 0,
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

std::unique_ptr<KlineVenue> xt(MarketType market) {
    // the catalogue lists perps and spot only
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Xt>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
