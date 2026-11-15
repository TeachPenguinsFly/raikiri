// blofin: GET /api/v1/market/candles, rows ["ts ms", "o", "h", "l", "c", "vol", "volCurrency", "volCurrencyQuote",
// "confirm"] newest first; vol is in contracts, the unit of a print's size, though cut to a whole contract
#include <algorithm>
#include <charconv>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Blofin final : public KlineVenue {
public:
    // after and before are both exclusive
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "openapi.blofin.com",
                .target = std::format("/api/v1/market/candles?instId={}&bar=1m&after={}&before={}&limit={}",
                                      symbol,
                                      to * 60'000LL,
                                      from * 60'000LL - 1,
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return 1440; }

    // a quarter of the 500 a minute the ip has
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view code;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get_string().get(code) != simdjson::SUCCESS || code != "0" ||
            root["data"].get_array().get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            if (!row.is_array() || row.get_array().value_unsafe().size() < 9) {
                continue;
            }
            const auto a = row.get_array().value_unsafe();
            std::string_view stamp;
            std::string_view confirm;
            std::int64_t open_ms{0};
            if (a.at(0).get_string().get(stamp) != simdjson::SUCCESS ||
                std::from_chars(stamp.data(), stamp.data() + stamp.size(), open_ms).ec != std::errc{} ||
                (a.at(8).get_string().get(confirm) == simdjson::SUCCESS && confirm == "0")) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_ms / 60'000),
                           .open = e9(a.at(1).value_unsafe()),
                           .high = e9(a.at(2).value_unsafe()),
                           .low = e9(a.at(3).value_unsafe()),
                           .close = e9(a.at(4).value_unsafe()),
                           .volume = e9(a.at(5).value_unsafe()),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto repeated = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(repeated.begin(), repeated.end());
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> blofin(MarketType market) {
    // the public api lists perpetuals only
    return market == MarketType::Perp ? std::make_unique<Blofin>() : nullptr;
}

} // namespace raikiri::candles::venues
