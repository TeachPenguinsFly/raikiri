// apex: GET /api/v3/klines, {"data":{"BTCUSDT":[{"t": open ms, "o", "h", "l", "c", "v", "tr"}]}} oldest first; v is
// in the base asset, which is what our apex prints count in, and tr is the quote turnover
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t number(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Apex final : public KlineVenue {
public:
    // start and end are inclusive seconds, and a range over the limit keeps its newest rows
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "omni.apex.exchange",
                .target = std::format("/api/v3/klines?symbol={}&interval=1&start={}&end={}&limit={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to) * 60 - 1,
                                      per_call())};
    }

    // a limit over 200 answers empty data rather than an error
    std::int32_t per_call() const noexcept override { return 200; }

    // 60 of the 600 a minute the ip has
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::object data;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["data"].get(data) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        // keyed by the one symbol asked for; an unknown symbol answers an empty object
        std::vector<Candle> out;
        for (const auto symbol : data) {
            simdjson::dom::array rows;
            if (symbol.value.get(rows) != simdjson::SUCCESS) {
                continue;
            }
            for (const auto item : rows) {
                simdjson::dom::object row;
                std::int64_t open_ms = 0;
                if (item.get(row) != simdjson::SUCCESS || row["t"].get(open_ms) != simdjson::SUCCESS) {
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
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> apex(MarketType market) {
    // apex omni lists perps only
    if (market != MarketType::Perp) {
        return nullptr;
    }
    return std::make_unique<Apex>();
}

} // namespace raikiri::candles::venues
