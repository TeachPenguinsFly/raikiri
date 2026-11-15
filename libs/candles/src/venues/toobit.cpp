// toobit: GET /quote/v1/klines for both markets, rows [open ms, "o", "h", "l", "c", "volume", 0, "quote", trades, ...];
// a perp's volume is in contracts and a spot pair's in the base asset, which is what our toobit prints count in
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Toobit final : public KlineVenue {
public:
    // both bounds are inclusive, and a range over the limit keeps its newest rows
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.toobit.com",
                .target = std::format("/quote/v1/klines?symbol={}&interval=1m&startTime={}&endTime={}&limit={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1,
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // weight 1 each, 60 of the 3000 a minute the ip has
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
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> toobit(MarketType market) {
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Toobit>();
}

} // namespace raikiri::candles::venues
