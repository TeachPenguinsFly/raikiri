// deribit: JSON-RPC POST of public/get_tradingview_chart_data, columns ticks (open ms), open, high, low, close, volume
// and cost, oldest first; an inverse contract's prints count dollars, which is cost, while linear perps and spot count
// the coin, which is volume. the answer echoes the request id, so the id names the column
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::vector<simdjson::dom::element> column(simdjson::dom::object result, std::string_view key) {
    std::vector<simdjson::dom::element> out;
    simdjson::dom::array values;
    if (result[key].get(values) == simdjson::SUCCESS) {
        for (const auto value : values) {
            out.push_back(value);
        }
    }
    return out;
}

class Deribit final : public KlineVenue {
public:
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        // linear and spot names carry their quote after an underscore, BTC_USDC-PERPETUAL; inverse ones never do
        const bool inverse = symbol.find('_') == std::string_view::npos;
        return {.host = "www.deribit.com",
                .target = "/api/v2",
                .body = std::format(R"({{"jsonrpc":"2.0","id":"{}","method":"public/get_tradingview_chart_data",)"
                                    R"("params":{{"instrument_name":"{}","start_timestamp":{},"end_timestamp":{},)"
                                    R"("resolution":"1"}}}})",
                                    inverse ? "cost" : "volume",
                                    symbol,
                                    static_cast<std::int64_t>(from) * 60'000,
                                    static_cast<std::int64_t>(to) * 60'000 - 1)};
    }

    // a longer stretch answers only its newest 5000 bars
    std::int32_t per_call() const noexcept override { return 5000; }

    // the gateway budgets 10 public calls a second of the ip's credits; 120 a minute is a fifth of that
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::object result;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["result"].get(result) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::string_view id;
        const bool dollars = root["id"].get(id) == simdjson::SUCCESS && id == "cost";
        const auto ticks = column(result, "ticks");
        const auto open = column(result, "open");
        const auto high = column(result, "high");
        const auto low = column(result, "low");
        const auto close = column(result, "close");
        const auto volume = column(result, dollars ? "cost" : "volume");
        const auto n = ticks.size();
        if (open.size() != n || high.size() != n || low.size() != n || close.size() != n || volume.size() != n) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::int64_t open_ms = 0;
            if (ticks[i].get(open_ms) != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_ms / 60'000),
                           .open = e9(open[i]),
                           .high = e9(high[i]),
                           .low = e9(low[i]),
                           .close = e9(close[i]),
                           .volume = e9(volume[i]),
                           .trades = 0,
                           .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> deribit(MarketType market) {
    // options print a few times a day across hundreds of strikes, not worth the ip's credits
    if (market != MarketType::Perp && market != MarketType::Future && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Deribit>();
}

} // namespace raikiri::candles::venues
