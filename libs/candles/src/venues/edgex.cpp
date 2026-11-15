// edgex: GET /api/v2/public/quote/getKline, data.dataList newest first with string fields; size is in the base asset,
// which is what our edgex prints count in. the route keys on the numeric contractId (30000001 is BTCUSDC) and refuses
// the name our catalogue keeps, so `symbol` here is that id from getMetaData
#include <algorithm>
#include <charconv>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t number(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

std::optional<std::int64_t> integer(simdjson::dom::object row, std::string_view key) noexcept {
    std::string_view text;
    std::int64_t out = 0;
    if (row[key].get(text) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return out;
}

class Edgex final : public KlineVenue {
public:
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "edgex-prod-v2.edgex.exchange",
                .target = std::format("/api/v2/public/quote/getKline?contractId={}&priceType=LAST_PRICE"
                                      "&klineType=MINUTE_1&size={}&filterBeginKlineTimeInclusive={}"
                                      "&filterEndKlineTimeExclusive={}",
                                      symbol,
                                      per_call(),
                                      static_cast<std::int64_t>(from) * 60'000,
                                      static_cast<std::int64_t>(to) * 60'000)};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // 60 a minute, nowhere near the 60 a second the ip has
    std::uint32_t per_minute() const noexcept override { return 60; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view code;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["code"].get(code) != simdjson::SUCCESS || code != "SUCCESS" ||
            root["data"]["dataList"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto item : rows) {
            simdjson::dom::object row;
            if (item.get(row) != simdjson::SUCCESS) {
                continue;
            }
            const auto open_ms = integer(row, "klineTime");
            if (!open_ms.has_value()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(*open_ms / 60'000),
                           .open = number(row, "open"),
                           .high = number(row, "high"),
                           .low = number(row, "low"),
                           .close = number(row, "close"),
                           .volume = number(row, "size"),
                           .trades = static_cast<std::int32_t>(integer(row, "trades").value_or(0)),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> edgex(MarketType market) {
    // spot getKline answers an empty list for every symbol id tried
    if (market != MarketType::Perp) {
        return nullptr;
    }
    return std::make_unique<Edgex>();
}

} // namespace raikiri::candles::venues
