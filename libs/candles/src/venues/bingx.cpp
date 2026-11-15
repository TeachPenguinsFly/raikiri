// bingx: perps GET /openApi/swap/v3/quote/klines, rows {"open", "close", "high", "low", "volume", "time": open ms};
// spot GET /openApi/spot/v2/market/kline, rows [open ms, o, h, l, c, volume, close ms, quote]; both newest first, the
// volume in the coin as the trade prints' q, and neither states a trade count
#include <algorithm>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t field(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Bingx final : public KlineVenue {
public:
    explicit Bingx(bool perp) : perp_{perp} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "open-api.bingx.com",
                .target = std::format("{}?symbol={}&interval=1m&startTime={}&endTime={}&limit={}",
                                      perp_ ? "/openApi/swap/v3/quote/klines" : "/openApi/spot/v2/market/kline",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1,
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return 1440; }

    // kept low: the market quota is per ip and the gateway polls it too
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::int64_t code = -1;
        simdjson::dom::array data;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get(code) != simdjson::SUCCESS || code != 0 || root["data"].get(data) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto entry : data) {
            if (perp_) {
                simdjson::dom::object row;
                std::int64_t open_ms = 0;
                if (entry.get(row) != simdjson::SUCCESS || row["time"].get(open_ms) != simdjson::SUCCESS) {
                    continue;
                }
                out.push_back({.instrument = instrument,
                               .minute = static_cast<std::int32_t>(open_ms / 60'000),
                               .open = field(row, "open"),
                               .high = field(row, "high"),
                               .low = field(row, "low"),
                               .close = field(row, "close"),
                               .volume = field(row, "volume"),
                               .trades = 0,
                               .source = Source::Venue});
                continue;
            }
            if (!entry.is_array() || entry.get_array().value_unsafe().size() < 6) {
                continue;
            }
            const auto a = entry.get_array().value_unsafe();
            const auto open_ms = a.at(0).get_int64();
            if (open_ms.error() != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_ms.value_unsafe() / 60'000),
                           .open = e9(a.at(1).value_unsafe()),
                           .high = e9(a.at(2).value_unsafe()),
                           .low = e9(a.at(3).value_unsafe()),
                           .close = e9(a.at(4).value_unsafe()),
                           .volume = e9(a.at(5).value_unsafe()),
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

std::unique_ptr<KlineVenue> bingx(MarketType market) {
    // the catalogue lists perps and spot only
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Bingx>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
