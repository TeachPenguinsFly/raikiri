// aster: binance's shape, GET /fapi/v1/klines and the spot /api/v1/klines, rows [open ms, "o", "h", "l", "c", "volume",
// close ms, "quote", trades, ...]; the volume is in the base asset, which is what our aster instruments count in
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Aster final : public KlineVenue {
public:
    explicit Aster(bool perp) : perp_{perp} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = perp_ ? "fapi.asterdex.com" : "sapi.asterdex.com",
                .target = std::format("{}?symbol={}&interval=1m&startTime={}&endTime={}&limit={}",
                                      perp_ ? "/fapi/v1/klines" : "/api/v1/klines",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1,
                                      per_call())};
    }

    // perps answer 1500 rows for weight 10, spot 1000 for weight 2
    std::int32_t per_call() const noexcept override { return perp_ ? 1500 : 1000; }

    // 30 calls of weight 10 is 300 of the 2400 a minute the ip has; spot 60 of weight 2 out of 6000
    std::uint32_t per_minute() const noexcept override { return perp_ ? 30 : 60; }

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
        return out;
    }

private:
    bool perp_;
};

} // namespace

std::unique_ptr<KlineVenue> aster(MarketType market) {
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Aster>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
