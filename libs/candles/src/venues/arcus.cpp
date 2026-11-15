// arcus: GET /v1/candles, newest first with openTime in microseconds; volume is in the base asset, as a print's size
#include <algorithm>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

constexpr std::int64_t k_minute_us = 60'000'000;

std::int64_t e9_at(simdjson::dom::element row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Arcus final : public KlineVenue {
public:
    // `to` takes in a bar that opens on it
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {
            .host = "api.arcus.xyz",
            .target = std::format(
                "/v1/candles?market={}&timeframe=1m&from={}&to={}", symbol, from * k_minute_us, to * k_minute_us - 1)};
    }

    // with no limit asked the answer is the newest 1500 bars of the stretch
    std::int32_t per_call() const noexcept override { return 1500; }

    // a quarter of the 60 calls a minute the ip is held to
    std::uint32_t per_minute() const noexcept override { return 15; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["candles"].get_array().get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            std::int64_t open_us{0};
            bool closed{true};
            if (row["openTime"].get_int64().get(open_us) != simdjson::SUCCESS ||
                (row["isFinal"].get_bool().get(closed) == simdjson::SUCCESS && !closed)) {
                continue;
            }
            const auto trades = row["tradeCount"].get_int64();
            out.push_back(
                {.instrument = instrument,
                 .minute = static_cast<std::int32_t>(open_us / k_minute_us),
                 .open = e9_at(row, "open"),
                 .high = e9_at(row, "high"),
                 .low = e9_at(row, "low"),
                 .close = e9_at(row, "close"),
                 .volume = e9_at(row, "volume"),
                 .trades = trades.error() == simdjson::SUCCESS ? static_cast<std::int32_t>(trades.value_unsafe()) : 0,
                 .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto repeated = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(repeated.begin(), repeated.end());
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> arcus(MarketType market) {
    // perpetuals only: arcus lists no spot
    return market == MarketType::Perp ? std::make_unique<Arcus>() : nullptr;
}

} // namespace raikiri::candles::venues
