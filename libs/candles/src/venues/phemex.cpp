// phemex: GET /exchange/public/md/v2/kline/list, data.rows [ts s, interval, last close, open, high, low, close,
// volume, turnover, symbol]. usdt and usdc perps send real prices and the volume in the base coin; the contracts whose
// symbol ends in usd send prices scaled by 1e4 and the volume in whole contracts. both are what our phemex prints count
#include <charconv>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

// every phemex usd contract prices in steps of 1e-4
constexpr std::int64_t k_scaled_price = 100'000;
constexpr std::int64_t k_contract = 1'000'000'000;

// an integer the venue sent as a string, times `scale`; zero when unreadable
std::int64_t scaled(simdjson::dom::element value, std::int64_t scale) noexcept {
    std::string_view text;
    if (value.get(text) != simdjson::SUCCESS) {
        return 0;
    }
    std::int64_t raw = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), raw);
    return ec == std::errc{} && end == text.data() + text.size() ? raw * scale : 0;
}

class Phemex final : public KlineVenue {
public:
    // seconds; to leaves out its own bucket
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.phemex.com",
                .target = std::format("/exchange/public/md/v2/kline/list?symbol={}&resolution=60&from={}&to={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to) * 60)};
    }

    // a wider window is refused with phemex.data.size.uplimt
    std::int32_t per_call() const noexcept override { return 2000; }

    // /public/ allows an ip 300 a minute and the gateway's catalogue reads share it
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::int64_t code = -1;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get(code) != simdjson::SUCCESS || code != 0 ||
            root["data"]["rows"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            if (!row.is_array() || row.get_array().value_unsafe().size() < 10) {
                continue;
            }
            const auto a = row.get_array().value_unsafe();
            const auto ts = a.at(0).get_int64();
            const auto symbol = a.at(9).get_string();
            if (ts.error() != simdjson::SUCCESS || symbol.error() != simdjson::SUCCESS) {
                continue;
            }
            Candle candle{.instrument = instrument,
                          .minute = static_cast<std::int32_t>(ts.value_unsafe() / 60),
                          .source = Source::Venue};
            if (symbol.value_unsafe().ends_with("USD")) {
                candle.open = scaled(a.at(3).value_unsafe(), k_scaled_price);
                candle.high = scaled(a.at(4).value_unsafe(), k_scaled_price);
                candle.low = scaled(a.at(5).value_unsafe(), k_scaled_price);
                candle.close = scaled(a.at(6).value_unsafe(), k_scaled_price);
                candle.volume = scaled(a.at(7).value_unsafe(), k_contract);
            } else {
                candle.open = e9(a.at(3).value_unsafe());
                candle.high = e9(a.at(4).value_unsafe());
                candle.low = e9(a.at(5).value_unsafe());
                candle.close = e9(a.at(6).value_unsafe());
                candle.volume = e9(a.at(7).value_unsafe());
            }
            out.push_back(candle);
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> phemex(MarketType market) {
    // spot volume is scaled by its base coin's value scale (2 to 8), which only the catalogue knows
    if (market != MarketType::Perp) {
        return nullptr;
    }
    return std::make_unique<Phemex>();
}

} // namespace raikiri::candles::venues
