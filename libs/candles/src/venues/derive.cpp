// derive: POST /public/get_tradingview_chart_data, bars of executed trades keyed by bucket start in seconds;
// volume_contracts is in the unit of a print's trade_amount, one contract being one unit of the base asset
#include <algorithm>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t e9_at(simdjson::dom::element row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Derive final : public KlineVenue {
public:
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.lyra.finance",
                .target = "/public/get_tradingview_chart_data",
                .body = std::format(R"({{"instrument_name":"{}","period":60,"start_timestamp":{},"end_timestamp":{}}})",
                                    symbol,
                                    from * 60LL,
                                    to * 60LL - 1)};
    }

    // the docs name no cap on one answer, so this stays modest
    std::int32_t per_call() const noexcept override { return 500; }

    // a fifth of the 300 a minute the ip has
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            (root.get_array().get(rows) != simdjson::SUCCESS &&
             root["result"].get_array().get(rows) != simdjson::SUCCESS)) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            std::int64_t bucket_s{0};
            if (row["timestamp_bucket"].get_int64().get(bucket_s) != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(bucket_s / 60),
                           .open = e9_at(row, "open_price"),
                           .high = e9_at(row, "high_price"),
                           .low = e9_at(row, "low_price"),
                           .close = e9_at(row, "close_price"),
                           .volume = e9_at(row, "volume_contracts"),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto repeated = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(repeated.begin(), repeated.end());
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> derive(MarketType market) {
    // options keep their own bars but are left out of history for now; derive lists no spot
    return market == MarketType::Perp ? std::make_unique<Derive>() : nullptr;
}

} // namespace raikiri::candles::venues
