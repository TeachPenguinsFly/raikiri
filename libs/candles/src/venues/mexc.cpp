// mexc: perps GET /api/v1/contract/kline/{symbol}, columns time (open s), open, close, high, low, vol, amount oldest
// first, vol in contracts as the deal prints' v; spot GET /api/v3/klines, rows [open ms, "o", "h", "l", "c", "volume",
// close ms, "quote"] with the volume in the base asset as the spot prints; neither states a trade count
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::vector<simdjson::dom::element> column(simdjson::dom::object data, std::string_view key) {
    std::vector<simdjson::dom::element> out;
    simdjson::dom::array values;
    if (data[key].get(values) == simdjson::SUCCESS) {
        for (const auto value : values) {
            out.push_back(value);
        }
    }
    return out;
}

class Mexc final : public KlineVenue {
public:
    explicit Mexc(bool perp) : perp_{perp} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        if (perp_) {
            // end is the open of the last bar wanted, inclusive
            return {.host = "contract.mexc.com",
                    .target = std::format("/api/v1/contract/kline/{}?interval=Min1&start={}&end={}",
                                          symbol,
                                          static_cast<std::int64_t>(from) * 60,
                                          static_cast<std::int64_t>(to - 1) * 60)};
        }
        return {.host = "api.mexc.com",
                .target = std::format("/api/v3/klines?symbol={}&interval=1m&startTime={}&endTime={}&limit={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1,
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return perp_ ? 2000 : 1000; }

    // perps allow 20 market calls each 2 s an ip, spot 500 each 10 s an endpoint; 120 a minute is a fifth of the less
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        return perp_ ? parse_perp(root, body, instrument) : parse_spot(root, body, instrument);
    }

private:
    static std::expected<std::vector<Candle>, std::string>
    parse_perp(simdjson::dom::element root, std::string_view body, InstrumentId instrument) {
        simdjson::dom::object data;
        if (root["data"].get(data) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        const auto time = column(data, "time");
        const auto open = column(data, "open");
        const auto high = column(data, "high");
        const auto low = column(data, "low");
        const auto close = column(data, "close");
        const auto vol = column(data, "vol");
        const auto n = time.size();
        if (open.size() != n || high.size() != n || low.size() != n || close.size() != n || vol.size() != n) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::int64_t open_s = 0;
            if (time[i].get(open_s) != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_s / 60),
                           .open = e9(open[i]),
                           .high = e9(high[i]),
                           .low = e9(low[i]),
                           .close = e9(close[i]),
                           .volume = e9(vol[i]),
                           .trades = 0,
                           .source = Source::Venue});
        }
        return out;
    }

    static std::expected<std::vector<Candle>, std::string>
    parse_spot(simdjson::dom::element root, std::string_view body, InstrumentId instrument) {
        if (!root.is_array()) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : root.get_array().value_unsafe()) {
            if (!row.is_array() || row.get_array().value_unsafe().size() < 6) {
                continue;
            }
            const auto a = row.get_array().value_unsafe();
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
        return out;
    }

    bool perp_;
};

} // namespace

std::unique_ptr<KlineVenue> mexc(MarketType market) {
    // the catalogue lists perps and spot only
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Mexc>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
