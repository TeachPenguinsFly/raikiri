// pacifica: GET /api/v1/kline, {"success":true,"data":[{"t": open ms, "o", "c", "h", "l", "v", "n"}]} oldest first, a
// minute without trades left out; v is in the base asset, which is what our pacifica prints count in
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t number(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Pacifica final : public KlineVenue {
public:
    // end_time includes a bar that opens on it
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.pacifica.fi",
                .target = std::format("/api/v1/kline?symbol={}&interval=1m&start_time={}&end_time={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1)};
    }

    // no limit parameter; a 2000 minute ask came back whole
    std::int32_t per_call() const noexcept override { return 1000; }

    // 20 of the 90 credits a minute our config gives the ip
    std::uint32_t per_minute() const noexcept override { return 20; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        bool success = false;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["success"].get(success) != simdjson::SUCCESS || !success ||
            root["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto item : rows) {
            simdjson::dom::object row;
            std::int64_t open_ms = 0;
            std::int64_t trades = 0;
            if (item.get(row) != simdjson::SUCCESS || row["t"].get(open_ms) != simdjson::SUCCESS) {
                continue;
            }
            if (row["n"].get(trades) != simdjson::SUCCESS) {
                trades = 0;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_ms / 60'000),
                           .open = number(row, "o"),
                           .high = number(row, "h"),
                           .low = number(row, "l"),
                           .close = number(row, "c"),
                           .volume = number(row, "v"),
                           .trades = static_cast<std::int32_t>(trades),
                           .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> pacifica(MarketType market) {
    // pacifica lists perps only
    if (market != MarketType::Perp) {
        return nullptr;
    }
    return std::make_unique<Pacifica>();
}

} // namespace raikiri::candles::venues
