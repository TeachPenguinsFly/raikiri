// okx: GET /api/v5/market/history-candles, rows ["ts ms", "o", "h", "l", "c", "vol", "volCcy", "volCcyQuote",
// "confirm"] newest first; vol is contracts on swaps and the base coin on spot, the unit of the trade sz our okx
// instruments count in
#include <algorithm>
#include <charconv>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

// the open time comes as a string of digits
std::optional<std::int64_t> integer(simdjson::dom::element value) noexcept {
    std::string_view text;
    if (value.get(text) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    std::int64_t n = 0;
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, n);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return n;
}

class Okx final : public KlineVenue {
public:
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        // after and before are both exclusive
        return {.host = "www.okx.com",
                .target = std::format("/api/v5/market/history-candles?instId={}&bar=1m&after={}&before={}&limit={}",
                                      symbol,
                                      static_cast<std::int64_t>(to) * 60'000,
                                      static_cast<std::int64_t>(from) * 60'000 - 1,
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return 100; }

    // the endpoint allows 20 calls per 2 s an ip, 600 a minute
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view code;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get(code) != simdjson::SUCCESS || code != "0" || root["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        out.reserve(rows.size());
        for (const auto row : rows) {
            simdjson::dom::array a;
            if (row.get(a) != simdjson::SUCCESS || a.size() < 9) {
                continue;
            }
            const auto open_ms = integer(a.at(0).value_unsafe());
            std::string_view confirm;
            // "0" is the minute still open
            if (!open_ms.has_value() || a.at(8).get(confirm) != simdjson::SUCCESS || confirm != "1") {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(*open_ms / 60'000),
                           .open = e9(a.at(1).value_unsafe()),
                           .high = e9(a.at(2).value_unsafe()),
                           .low = e9(a.at(3).value_unsafe()),
                           .close = e9(a.at(4).value_unsafe()),
                           .volume = e9(a.at(5).value_unsafe()),
                           .trades = 0,
                           .source = Source::Venue});
        }
        std::ranges::reverse(out);
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> okx(MarketType market) {
    // our okx catalogue holds no dated futures, and option minutes are left to the live feed
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Okx>();
}

} // namespace raikiri::candles::venues
