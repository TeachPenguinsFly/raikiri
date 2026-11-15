// bitstamp: GET /api/v2/ohlc/{market}/, {"data": {"ohlc": [{timestamp s, open, high, low, close, volume}]}}; the
// volume is in the base asset, as is the amount of our bitstamp prints, perps (btcusd-perp) included
#include <charconv>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Bitstamp final : public KlineVenue {
public:
    // an end would win over the start and count back from it, so the stretch is a start and a count
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {
            .host = "www.bitstamp.net",
            .target = std::format(
                "/api/v2/ohlc/{}/?step=60&limit={}&start={}", symbol, to - from, static_cast<std::int64_t>(from) * 60)};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // the ip may spend 8000 calls in ten minutes
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["data"]["ohlc"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            std::string_view stamp;
            simdjson::dom::element o;
            simdjson::dom::element h;
            simdjson::dom::element l;
            simdjson::dom::element c;
            simdjson::dom::element v;
            if (row["timestamp"].get(stamp) != simdjson::SUCCESS || row["open"].get(o) != simdjson::SUCCESS ||
                row["high"].get(h) != simdjson::SUCCESS || row["low"].get(l) != simdjson::SUCCESS ||
                row["close"].get(c) != simdjson::SUCCESS || row["volume"].get(v) != simdjson::SUCCESS) {
                continue;
            }
            std::int64_t seconds = 0;
            const auto [end, ec] = std::from_chars(stamp.data(), stamp.data() + stamp.size(), seconds);
            if (ec != std::errc{} || end != stamp.data() + stamp.size()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(seconds / 60),
                           .open = e9(o),
                           .high = e9(h),
                           .low = e9(l),
                           .close = e9(c),
                           .volume = e9(v),
                           .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> bitstamp(MarketType market) {
    if (market != MarketType::Perp && market != MarketType::Spot) {
        return nullptr; // bitstamp lists no dated futures or options
    }
    return std::make_unique<Bitstamp>();
}

} // namespace raikiri::candles::venues
