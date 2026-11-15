// gate: GET /api/v4/futures/usdt/candlesticks, rows {"t": s, "v": contracts, "o", "h", "l", "c", "sum"}, and
// /api/v4/spot/candlesticks, rows ["t s", "quote volume", "c", "h", "l", "o", "base volume", "closed"], both oldest
// first; the volume is contracts on perps and the base coin on spot, as the trade size and amount our gate instruments
// count in
#include <charconv>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

// a perp row states its time as a number, a spot row as a string
std::optional<std::int64_t> integer(simdjson::simdjson_result<simdjson::dom::element> value) noexcept {
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

std::int64_t field(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Gate final : public KlineVenue {
public:
    explicit Gate(bool perp) : perp_{perp} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        // from and to are both inclusive
        return {.host = "api.gateio.ws",
                .target = std::format("{}={}&interval=1m&from={}&to={}",
                                      perp_ ? "/api/v4/futures/usdt/candlesticks?contract"
                                            : "/api/v4/spot/candlesticks?currency_pair",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to) * 60 - 1)};
    }

    std::int32_t per_call() const noexcept override { return perp_ ? 2000 : 1000; }

    // each market endpoint allows 200 calls per 10 s an ip, 1200 a minute
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root.get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        out.reserve(rows.size());
        for (const auto row : rows) {
            if (perp_) {
                simdjson::dom::object o;
                if (row.get(o) != simdjson::SUCCESS) {
                    continue;
                }
                const auto start_s = integer(o["t"]);
                if (!start_s.has_value()) {
                    continue;
                }
                out.push_back({.instrument = instrument,
                               .minute = static_cast<std::int32_t>(*start_s / 60),
                               .open = field(o, "o"),
                               .high = field(o, "h"),
                               .low = field(o, "l"),
                               .close = field(o, "c"),
                               .volume = field(o, "v"),
                               .trades = 0,
                               .source = Source::Venue});
                continue;
            }
            simdjson::dom::array a;
            if (row.get(a) != simdjson::SUCCESS || a.size() < 7) {
                continue;
            }
            const auto start_s = integer(a.at(0));
            std::string_view closed{"true"};
            // "false" is the minute still open
            if (a.size() > 7 && a.at(7).get(closed) != simdjson::SUCCESS) {
                continue;
            }
            if (!start_s.has_value() || closed != "true") {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(*start_s / 60),
                           .open = e9(a.at(5).value_unsafe()),
                           .high = e9(a.at(3).value_unsafe()),
                           .low = e9(a.at(4).value_unsafe()),
                           .close = e9(a.at(2).value_unsafe()),
                           .volume = e9(a.at(6).value_unsafe()),
                           .trades = 0,
                           .source = Source::Venue});
        }
        return out;
    }

private:
    bool perp_;
};

} // namespace

std::unique_ptr<KlineVenue> gate(MarketType market) {
    // our gate catalogue loads usdt perps and spot only; delivery contracts are not loaded
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Gate>(market == MarketType::Perp);
}

} // namespace raikiri::candles::venues
