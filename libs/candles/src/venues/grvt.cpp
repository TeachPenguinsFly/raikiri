// grvt: POST /full/v1/kline, newest first with open_time in ns; volume_b is in the base asset, as a print's size
#include <algorithm>
#include <charconv>
#include <optional>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

constexpr std::int64_t k_minute_ns = 60'000'000'000;

std::int64_t e9_at(simdjson::dom::element row, std::string_view key) noexcept {
    simdjson::dom::element value;
    return row[key].get(value) == simdjson::SUCCESS ? e9(value) : 0;
}

std::optional<std::int64_t> whole_at(simdjson::dom::element row, std::string_view key) noexcept {
    simdjson::dom::element value;
    if (row[key].get(value) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    if (value.is_int64()) {
        return value.get_int64().value_unsafe();
    }
    std::string_view text;
    std::int64_t out{0};
    if (value.get(text) != simdjson::SUCCESS ||
        std::from_chars(text.data(), text.data() + text.size(), out).ec != std::errc{}) {
        return std::nullopt;
    }
    return out;
}

class Grvt final : public KlineVenue {
public:
    // the end takes in a bar that opens on it
    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to) const override {
        return {.host = "market-data.grvt.io",
                .target = "/full/v1/kline",
                .body = std::format(R"({{"instrument":"{}","interval":"CI_1_M","type":"TRADE","start_time":"{}",)"
                                    R"("end_time":"{}","limit":{}}})",
                                    symbol,
                                    from * k_minute_ns,
                                    to * k_minute_ns - 1,
                                    per_call())};
    }

    std::int32_t per_call() const noexcept override { return 1000; }

    // a quarter of the 500 reads a minute market data allows outside the instrument list
    std::uint32_t per_minute() const noexcept override { return 120; }

    std::expected<std::vector<Candle>, std::string> parse(std::string_view body,
                                                          InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array rows;
        if (parser.parse(body.data(), body.size()).get(root) != simdjson::SUCCESS ||
            root["result"].get_array().get(rows) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<Candle> out;
        for (const auto row : rows) {
            const auto open_ns = whole_at(row, "open_time");
            if (!open_ns.has_value()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(*open_ns / k_minute_ns),
                           .open = e9_at(row, "open"),
                           .high = e9_at(row, "high"),
                           .low = e9_at(row, "low"),
                           .close = e9_at(row, "close"),
                           .volume = e9_at(row, "volume_b"),
                           .trades = static_cast<std::int32_t>(whole_at(row, "trades").value_or(0)),
                           .source = Source::Venue});
        }
        std::ranges::sort(out, {}, &Candle::minute);
        const auto repeated = std::ranges::unique(out, {}, &Candle::minute);
        out.erase(repeated.begin(), repeated.end());
        return out;
    }
};

} // namespace

std::unique_ptr<KlineVenue> grvt(MarketType market) {
    // perpetuals only: grvt lists no spot
    return market == MarketType::Perp ? std::make_unique<Grvt>() : nullptr;
}

} // namespace raikiri::candles::venues
