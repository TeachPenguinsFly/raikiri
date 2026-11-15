// bybit: GET /v5/market/kline, rows ["start ms", "o", "h", "l", "c", "volume", "turnover"] newest first; the volume is
// the base coin on linear and spot, the unit of the trade v our bybit instruments count in
#include <algorithm>
#include <charconv>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

// the start comes as a string of digits
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

class Bybit final : public KlineVenue {
public:
    explicit Bybit(std::string_view category) : category_{category} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        // start and end are both inclusive
        return {.host = "api.bybit.com",
                .target = std::format("/v5/market/kline?category={}&symbol={}&interval=1&start={}&end={}&limit={}",
                                      category_,
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000 - 1,
                                      per_call())};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // the ip may send 600 calls per 5 s in all; 120 a minute leaves nearly all of it to the gateway
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::int64_t code = -1;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["retCode"].get(code) != simdjson::SUCCESS || code != 0 ||
            root["result"]["list"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        out.reserve(rows.size());
        for (const auto row : rows) {
            simdjson::dom::array a;
            if (row.get(a) != simdjson::SUCCESS || a.size() < 6) {
                continue;
            }
            const auto start_ms = integer(a.at(0).value_unsafe());
            if (!start_ms.has_value()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(*start_ms / 60'000),
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

private:
    std::string_view category_;
};

} // namespace

std::unique_ptr<KlineVenue> bybit(MarketType market) {
    // dated futures sit in the linear category beside the perps; options have no kline
    if (market == MarketType::Perp || market == MarketType::Future) {
        return std::make_unique<Bybit>("linear");
    }
    if (market == MarketType::Spot) {
        return std::make_unique<Bybit>("spot");
    }
    return nullptr;
}

} // namespace raikiri::candles::venues
