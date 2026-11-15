// hyperliquid: POST /info candleSnapshot, rows {"t": open ms, "T", "s", "i", "o", "c", "h", "l", "v", "n"} oldest
// first; v is in the coin, as the trades' sz our feed folds, and only the newest 5000 bars are kept
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t field(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Hyperliquid final : public KlineVenue {
public:
    // the coin is taken as listed: "BTC", a dex perp's "xyz:TSLA", a spot pair's "@107"
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.hyperliquid.xyz",
                .target = "/info",
                .body = std::format(R"({{"type":"candleSnapshot","req":{{"coin":"{}","interval":"1m",)"
                                    R"("startTime":{},"endTime":{}}}}})",
                                    symbol,
                                    static_cast<std::int64_t>(from) * 60'000,
                                    static_cast<std::int64_t>(to) * 60'000 - 1)};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // an info call weighs 20 and one more per 60 bars: 8 full calls are about 300 of the 1200 a minute the ip has
    std::uint32_t per_minute() const noexcept override { return 8; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS || !root.is_array()) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        const auto entry_list = root.get_array().value_unsafe();
        for (const auto entry : entry_list) {
            simdjson::dom::object row;
            std::int64_t open_ms = 0;
            if (entry.get(row) != simdjson::SUCCESS || row["t"].get(open_ms) != simdjson::SUCCESS) {
                continue;
            }
            const auto trades = row["n"].get_int64();
            out.push_back(
                {.instrument = instrument,
                 .minute = static_cast<std::int32_t>(open_ms / 60'000),
                 .open = field(row, "o"),
                 .high = field(row, "h"),
                 .low = field(row, "l"),
                 .close = field(row, "c"),
                 .volume = field(row, "v"),
                 .trades = trades.error() == simdjson::SUCCESS ? static_cast<std::int32_t>(trades.value_unsafe()) : 0,
                 .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> hyperliquid(MarketType market) {
    // the catalogue lists perps, dex perps among them, and spot pairs only
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Hyperliquid>();
}

} // namespace raikiri::candles::venues
