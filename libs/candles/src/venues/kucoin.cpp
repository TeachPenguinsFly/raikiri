// kucoin: GET api-futures /api/v1/kline/query, rows [ts ms, o, h, l, c, volume, turnover] as numbers oldest first, and
// api /api/v1/market/candles, rows ["ts s", "o", "c", "h", "l", "volume", "turnover"] newest first; the futures volume
// is lots and the spot volume the base coin, the unit of the trade size our kucoin instruments count in
#include <raikiri/common/fixed_point.hpp>

#include <algorithm>
#include <charconv>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

// futures send the open time as a number, spot as a string
std::optional<std::int64_t> integer(simdjson::dom::element value) noexcept {
    std::int64_t n = 0;
    if (value.get(n) == simdjson::SUCCESS) {
        return n;
    }
    std::string_view text;
    if (value.get(text) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, n);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return n;
}

// a spot volume is a sum carrying float noise past 1e-9, which e9 refuses
std::int64_t volume_e9(simdjson::dom::element value) noexcept {
    std::string_view text;
    if (value.get(text) != simdjson::SUCCESS) {
        return e9(value);
    }
    const auto parsed = parse_amount_e9(text);
    return parsed.has_value() ? parsed->raw : 0;
}

class Kucoin final : public KlineVenue {
public:
    explicit Kucoin(bool perp) : perp_{perp} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        // both ends are inclusive: milliseconds on futures, seconds on spot
        if (perp_) {
            return {.host = "api-futures.kucoin.com",
                    .target = std::format("/api/v1/kline/query?symbol={}&granularity=1&from={}&to={}",
                                          symbol,
                                          static_cast<std::int64_t>(from) * 60'000,
                                          static_cast<std::int64_t>(to) * 60'000 - 1)};
        }
        return {.host = "api.kucoin.com",
                .target = std::format("/api/v1/market/candles?type=1min&symbol={}&startAt={}&endAt={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to) * 60 - 1)};
    }

    std::int32_t per_call() const noexcept override { return perp_ ? 500 : 1500; }

    // a kline call weighs 3 of the 2000 public weight each host gives an ip per 30 s; 60 a minute spends 180 of 4000
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view code;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get(code) != simdjson::SUCCESS || code != "200000" ||
            root["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        // spot columns run open, close, high, low
        const std::size_t high_at = perp_ ? 2 : 3;
        const std::size_t low_at = perp_ ? 3 : 4;
        const std::size_t close_at = perp_ ? 4 : 2;
        const std::int64_t unit = perp_ ? 60'000 : 60;
        std::vector<Candle> out;
        out.reserve(rows.size());
        for (const auto row : rows) {
            simdjson::dom::array a;
            if (row.get(a) != simdjson::SUCCESS || a.size() < 6) {
                continue;
            }
            const auto open_time = integer(a.at(0).value_unsafe());
            if (!open_time.has_value()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(*open_time / unit),
                           .open = e9(a.at(1).value_unsafe()),
                           .high = e9(a.at(high_at).value_unsafe()),
                           .low = e9(a.at(low_at).value_unsafe()),
                           .close = e9(a.at(close_at).value_unsafe()),
                           .volume = volume_e9(a.at(5).value_unsafe()),
                           .trades = 0,
                           .source = Source::Venue});
        }
        if (!perp_) {
            std::ranges::reverse(out);
        }
        return out;
    }

private:
    bool perp_;
};

} // namespace

std::unique_ptr<KlineVenue> kucoin(MarketType market) {
    // our kucoin catalogue models no dated futures (all inverse) and no options
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Kucoin>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
