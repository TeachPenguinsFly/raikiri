// bitget: GET /api/v2/mix/market/history-candles and /api/v2/spot/market/history-candles, rows ["ts ms", "o", "h", "l",
// "c", "base volume", "quote volume", ...] oldest first; the base volume is the unit of the trade size our bitget usdt
// perps and spot count in
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

class Bitget final : public KlineVenue {
public:
    explicit Bitget(bool perp) : perp_{perp} {}

    // endTime is exclusive and the answer is the `limit` minutes before it; a startTime beside it moves the window a
    // minute early, and the spot endpoint takes none
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        const auto end = static_cast<std::int64_t>(to) * 60'000;
        const auto limit = std::clamp(to - from, 1, per_call());
        return {.host = "api.bitget.com",
                .target = perp_ ? std::format("/api/v2/mix/market/history-candles?symbol={}&productType=USDT-FUTURES"
                                              "&granularity=1m&endTime={}&limit={}",
                                              symbol,
                                              end,
                                              limit)
                                : std::format("/api/v2/spot/market/history-candles?symbol={}&granularity=1min"
                                              "&endTime={}&limit={}",
                                              symbol,
                                              end,
                                              limit)};
    }

    std::int32_t per_call() const noexcept override { return 200; }

    // each history endpoint allows 20 calls a second an ip, 1200 a minute
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view code;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get(code) != simdjson::SUCCESS || code != "00000" ||
            root["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        out.reserve(rows.size());
        for (const auto row : rows) {
            simdjson::dom::array a;
            if (row.get(a) != simdjson::SUCCESS || a.size() < 6) {
                continue;
            }
            const auto open_ms = integer(a.at(0).value_unsafe());
            if (!open_ms.has_value()) {
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
        return out;
    }

private:
    bool perp_;
};

} // namespace

std::unique_ptr<KlineVenue> bitget(MarketType market) {
    // our bitget catalogue loads usdt perps and spot only
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Bitget>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
