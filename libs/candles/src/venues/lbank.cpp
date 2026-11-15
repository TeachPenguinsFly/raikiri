// lbank: GET /v2/kline.do on the spot api, rows [open s, o, h, l, c, volume] as numbers, a quiet minute kept flat; the
// volume is in the base asset, which is what our spot prints count in
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

class Lbank final : public KlineVenue {
public:
    // the answer runs forward from `time` with no end bound, so the size stops it at `to`
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.lbank.info",
                .target = std::format("/v2/kline.do?symbol={}&size={}&type=minute1&time={}",
                                      symbol,
                                      to - from,
                                      static_cast<std::int64_t>(from) * 60)};
    }

    // 2000 is the documented most, but a page that size streams for tens of seconds
    std::int32_t per_call() const noexcept override { return 500; }

    // 30 of the 1200 a minute the ip has on /v2/
    std::uint32_t per_minute() const noexcept override { return 30; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view result;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["result"].get(result) != simdjson::SUCCESS || result != "true" ||
            root["data"].get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            if (!row.is_array() || row.get_array().value_unsafe().size() < 6) {
                continue;
            }
            const auto a = row.get_array().value_unsafe();
            const auto open_s = a.at(0).get_int64();
            if (open_s.error() != simdjson::SUCCESS) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(open_s.value_unsafe() / 60),
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
};

} // namespace

std::unique_ptr<KlineVenue> lbank(MarketType market) {
    // the cfd api behind our perps has no kline route
    if (market != MarketType::Spot) {
        return nullptr;
    }
    return std::make_unique<Lbank>();
}

} // namespace raikiri::candles::venues
