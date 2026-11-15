// htx: GET /linear-swap-ex/market/history/kline, rows {"id": open s, "open", "close", "high", "low", "amount", "vol",
// "trade_turnover", "count"} oldest first; vol counts contracts, as the trade prints' amount our feed folds
#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

std::int64_t field(simdjson::dom::object row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

class Htx final : public KlineVenue {
public:
    // to is the open of the last bar wanted, inclusive
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "api.hbdm.vn",
                .target = std::format("/linear-swap-ex/market/history/kline?contract_code={}&period=1min&from={}&to={}",
                                      symbol,
                                      static_cast<std::int64_t>(from) * 60,
                                      static_cast<std::int64_t>(to - 1) * 60)};
    }

    // a from/to stretch over 2000 bars is refused
    std::int32_t per_call() const noexcept override { return 2000; }

    // the ip has 240 public calls each 3 s; 120 a minute is a fortieth of it
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        std::string_view status;
        simdjson::dom::array data;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["status"].get(status) != simdjson::SUCCESS || status != "ok" ||
            root["data"].get(data) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto entry : data) {
            simdjson::dom::object row;
            std::int64_t open_s = 0;
            if (entry.get(row) != simdjson::SUCCESS || row["id"].get(open_s) != simdjson::SUCCESS) {
                continue;
            }
            const auto trades = row["count"].get_int64();
            out.push_back(
                {.instrument = instrument,
                 .minute = static_cast<std::int32_t>(open_s / 60),
                 .open = field(row, "open"),
                 .high = field(row, "high"),
                 .low = field(row, "low"),
                 .close = field(row, "close"),
                 .volume = field(row, "vol"),
                 .trades = trades.error() == simdjson::SUCCESS ? static_cast<std::int32_t>(trades.value_unsafe()) : 0,
                 .source = Source::Venue});
        }
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> htx(MarketType market) {
    // spot's history/kline takes no time range, only the newest bars, so a past gap cannot be asked for
    if (market != MarketType::Perp) {
        return nullptr;
    }
    return std::make_unique<Htx>();
}

} // namespace raikiri::candles::venues
