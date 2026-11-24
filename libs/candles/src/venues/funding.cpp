// settled funding of the venues' perps, one row per funding time:
//   binance      GET /fapi/v1/fundingRate, [{"fundingTime": ms, "fundingRate": "r", ...}] oldest first from startTime
//   bybit        GET /v5/market/funding/history, result.list [{"fundingRateTimestamp": "ms", "fundingRate": "r"}]
//                newest first
//   okx          GET /api/v5/public/funding-rate-history, data [{"fundingTime": "ms", "realizedRate": "r", ...}] newest
//                first
//   hyperliquid  POST /info fundingHistory, [{"time": ms, "fundingRate": "r", ...}] oldest first from startTime
//   gate         GET /api/v4/futures/usdt/funding_rate, [{"t": s, "r": "r"}] newest first
//   bitget       GET /api/v2/mix/market/history-fund-rate, data [{"fundingTime": "ms", "fundingRate": "r"}] newest
//                first, by page
//   kucoin       GET api-futures /api/v1/contract/funding-rates, data [{"timepoint": ms, "fundingRate": r}] newest
//   first
// The rate is the fraction paid per interval, as the gateways' funding cells hold it
#include <raikiri/candles/funding.hpp>
#include <raikiri/common/fixed_point.hpp>

#include <algorithm>
#include <charconv>

#include "klines.hpp"

namespace raikiri::candles::venues {

namespace {

using More = FundingVenue::More;

struct Spec {
    KlineCall (*call)(std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t page);
    std::size_t per_call;
    More more;
    std::uint32_t per_minute;
    // where the rows sit under the root, each key empty when unused
    std::string_view outer;
    std::string_view inner;
    std::string_view time;
    std::string_view rate;
    std::int64_t time_per_minute;
};

[[nodiscard]] std::int64_t ms(std::int32_t minute) noexcept {
    return static_cast<std::int64_t>(minute) * 60'000;
}

// a time comes as a number or as a string of digits
[[nodiscard]] std::optional<std::int64_t> integer(simdjson::dom::element value) noexcept {
    if (std::int64_t n = 0; value.get(n) == simdjson::SUCCESS) {
        return n;
    }
    std::string_view text;
    std::int64_t n = 0;
    if (value.get(text) != simdjson::SUCCESS || text.empty() ||
        std::from_chars(text.data(), text.data() + text.size(), n).ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return n;
}

// okx and hyperliquid print a rate past nine places, which is cut there
[[nodiscard]] std::optional<std::int64_t> rate_e9(simdjson::dom::element value) noexcept {
    if (std::string_view text; value.get(text) == simdjson::SUCCESS) {
        const auto parsed = parse_rate_e9(text);
        return parsed.has_value() ? std::optional{parsed->raw} : std::nullopt;
    }
    if (value.is_number()) {
        return e9(value);
    }
    return std::nullopt;
}

class Rest final : public FundingVenue {
public:
    explicit Rest(const Spec& spec) : spec_{spec} {}

    KlineCall call(std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t page) const override {
        return spec_.call(symbol, from, to, page);
    }

    std::size_t per_call() const noexcept override { return spec_.per_call; }

    More more() const noexcept override { return spec_.more; }

    std::uint32_t per_minute() const noexcept override { return spec_.per_minute; }

    std::expected<std::vector<MetricSample>, std::string> parse(std::string_view body,
                                                                InstrumentId instrument) const override {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        simdjson::dom::array list;
        auto error = parser.parse(body.data(), body.size()).get(root);
        if (error == simdjson::SUCCESS && !spec_.outer.empty()) {
            error = root[spec_.outer].get(root);
        }
        if (error == simdjson::SUCCESS && !spec_.inner.empty()) {
            error = root[spec_.inner].get(root);
        }
        if (error != simdjson::SUCCESS || root.get(list) != simdjson::SUCCESS) {
            return std::unexpected{std::string{body.substr(0, 160)}};
        }
        std::vector<MetricSample> out;
        for (const auto entry : list) {
            simdjson::dom::element time;
            simdjson::dom::element rate;
            if (entry[spec_.time].get(time) != simdjson::SUCCESS || entry[spec_.rate].get(rate) != simdjson::SUCCESS) {
                continue;
            }
            const auto at = integer(time);
            const auto value = rate_e9(rate);
            if (!at.has_value() || !value.has_value()) {
                continue;
            }
            out.push_back({.instrument = instrument,
                           .minute = static_cast<std::int32_t>(*at / spec_.time_per_minute),
                           .funding_rate = value});
        }
        return out;
    }

private:
    Spec spec_;
};

// the endpoint takes 500 calls per 5 min an ip whatever their size; the hourly contracts' 30 days are 720 rows
constexpr Spec k_binance{
    .call =
        [](std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t) {
            return KlineCall{
                .host = "fapi.binance.com",
                .target = std::format(
                    "/fapi/v1/fundingRate?symbol={}&startTime={}&endTime={}&limit=1000", symbol, ms(from), ms(to) - 1)};
        },
    .per_call = 1000,
    .more = More::Newer,
    .per_minute = 20,
    .outer = {},
    .inner = {},
    .time = "fundingTime",
    .rate = "fundingRate",
    .time_per_minute = 60'000,
};

constexpr Spec k_bybit{
    .call =
        [](std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t) {
            return KlineCall{
                .host = "api.bybit.com",
                .target = std::format("/v5/market/funding/history?category=linear&symbol={}&startTime={}&endTime={}"
                                      "&limit=200",
                                      symbol,
                                      ms(from),
                                      ms(to) - 1)};
        },
    .per_call = 200,
    .more = More::Older,
    .per_minute = 20,
    .outer = "result",
    .inner = "list",
    .time = "fundingRateTimestamp",
    .rate = "fundingRate",
    .time_per_minute = 60'000,
};

// after and before are both exclusive. realizedRate is what was paid; fundingRate beside it is the forecast
constexpr Spec k_okx{
    .call =
        [](std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t) {
            return KlineCall{
                .host = "www.okx.com",
                .target = std::format("/api/v5/public/funding-rate-history?instId={}&after={}&before={}&limit=100",
                                      symbol,
                                      ms(to),
                                      ms(from) - 1)};
        },
    .per_call = 100,
    .more = More::Older,
    .per_minute = 20,
    .outer = "data",
    .inner = {},
    .time = "fundingTime",
    .rate = "realizedRate",
    .time_per_minute = 60'000,
};

// funding is hourly and an answer stops at 500 rows; a full one weighs about 45 of the 1200 a minute the ip has
constexpr Spec k_hyperliquid{
    .call =
        [](std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t) {
            return KlineCall{
                .host = "api.hyperliquid.xyz",
                .target = "/info",
                .body = std::format(R"({{"type":"fundingHistory","coin":"{}","startTime":{},"endTime":{}}})",
                                    symbol,
                                    ms(from),
                                    ms(to) - 1)};
        },
    .per_call = 500,
    .more = More::Newer,
    .per_minute = 6,
    .outer = {},
    .inner = {},
    .time = "time",
    .rate = "fundingRate",
    .time_per_minute = 60'000,
};

// the newest `limit` rows, 1000 at most: an hourly contract's window fits in one answer
constexpr Spec k_gate{
    .call =
        [](std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t) {
            return KlineCall{.host = "api.gateio.ws",
                             .target = std::format("/api/v4/futures/usdt/funding_rate?contract={}&limit={}",
                                                   symbol,
                                                   std::clamp((to - from) / 60 + 1, 1, 1000))};
        },
    .per_call = 1000,
    .more = More::None,
    .per_minute = 20,
    .outer = {},
    .inner = {},
    .time = "t",
    .rate = "r",
    .time_per_minute = 60,
};

// the endpoint takes no time window, only pages of 100 from the newest back
constexpr Spec k_bitget{
    .call =
        [](std::string_view symbol, std::int32_t, std::int32_t, std::int32_t page) {
            return KlineCall{.host = "api.bitget.com",
                             .target = std::format("/api/v2/mix/market/history-fund-rate?symbol={}"
                                                   "&productType=USDT-FUTURES&pageSize=100&pageNo={}",
                                                   symbol,
                                                   page)};
        },
    .per_call = 100,
    .more = More::Page,
    .per_minute = 20,
    .outer = "data",
    .inner = {},
    .time = "fundingTime",
    .rate = "fundingRate",
    .time_per_minute = 60'000,
};

// both ends inclusive; an answer stops at the newest 100 rows of the window
constexpr Spec k_kucoin{
    .call =
        [](std::string_view symbol, std::int32_t from, std::int32_t to, std::int32_t) {
            return KlineCall{
                .host = "api-futures.kucoin.com",
                .target = std::format(
                    "/api/v1/contract/funding-rates?symbol={}&from={}&to={}", symbol, ms(from), ms(to) - 1)};
        },
    .per_call = 100,
    .more = More::Older,
    .per_minute = 20,
    .outer = "data",
    .inner = {},
    .time = "timepoint",
    .rate = "fundingRate",
    .time_per_minute = 60'000,
};

} // namespace

} // namespace raikiri::candles::venues

namespace raikiri::candles {

std::unique_ptr<FundingVenue> funding_venue(VenueId venue, MarketType market) {
    // the perps each catalogue loads: usd-m on binance, linear on bybit, usdt-margined on gate, bitget and kucoin
    if (market != MarketType::Perp) {
        return nullptr;
    }
    switch (venue) {
    case VenueId::Binance:
        return std::make_unique<venues::Rest>(venues::k_binance);
    case VenueId::Bybit:
        return std::make_unique<venues::Rest>(venues::k_bybit);
    case VenueId::Okx:
        return std::make_unique<venues::Rest>(venues::k_okx);
    case VenueId::Hyperliquid:
        return std::make_unique<venues::Rest>(venues::k_hyperliquid);
    case VenueId::Gate:
        return std::make_unique<venues::Rest>(venues::k_gate);
    case VenueId::Bitget:
        return std::make_unique<venues::Rest>(venues::k_bitget);
    case VenueId::Kucoin:
        return std::make_unique<venues::Rest>(venues::k_kucoin);
    default:
        return nullptr;
    }
}

} // namespace raikiri::candles
