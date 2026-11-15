// extended: GET /api/v1/info/candles/{market}/trades, {"status":"OK","data":[{"o","l","h","c","v","T": open ms}]}
// newest first; v is in the market's own asset, which is what our extended prints count in
#include <algorithm>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t number(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Extended final : public KlineVenue {
public:
    // no start bound: it pages back from an inclusive endTime, a quiet minute kept with zero volume, so the limit
    // stops it at `from`
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.starknet.extended.exchange",
                .target = std::format("/api/v1/info/candles/{}/trades?interval=PT1M&limit={}&endTime={}",
                                      symbol,
                                      to - from,
                                      static_cast<std::int64_t>(to) * 60'000 - 1)};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // 60 of the 1000 a minute the ip has
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view status;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["status"].get(status) != simdjson::SUCCESS || status != "OK" ||
            root["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto item : rows) {
            simdjson::dom::object row;
            std::int64_t open_ms = 0;
            if (item.get(row) != simdjson::SUCCESS || row["T"].get(open_ms) != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_ms / 60'000),
                           .open = number(row, "o"),
                           .high = number(row, "h"),
                           .low = number(row, "l"),
                           .close = number(row, "c"),
                           .volume = number(row, "v"),
                           .trades = 0,
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> extended(MarketType market) {
    // extended lists perps only
    if (market != MarketType::Perp) {
        return nullptr;
    }
    return std::make_unique<Extended>();
}

} // namespace raikiri::candles::venues
