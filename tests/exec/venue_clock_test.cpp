#include <raikiri/common/time.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/apex/apex_venue.hpp>
#include <raikiri/venues/aster/aster_venue.hpp>
#include <raikiri/venues/backpack/backpack_venue.hpp>
#include <raikiri/venues/binance/binance_venue.hpp>
#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/bithumb/bithumb_venue.hpp>
#include <raikiri/venues/bitstamp/bitstamp_venue.hpp>
#include <raikiri/venues/bitunix/bitunix_venue.hpp>
#include <raikiri/venues/blofin/blofin_venue.hpp>
#include <raikiri/venues/bybit/bybit_venue.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/cryptocom/cryptocom_venue.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/derive/derive_venue.hpp>
#include <raikiri/venues/dydx/dydx_venue.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>
#include <raikiri/venues/extended/extended_venue.hpp>
#include <raikiri/venues/gate/gate_venue.hpp>
#include <raikiri/venues/grvt/grvt_venue.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/lbank/lbank_venue.hpp>
#include <raikiri/venues/lighter/lighter_venue.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/okx/okx_venue.hpp>
#include <raikiri/venues/pacifica/pacifica_venue.hpp>
#include <raikiri/venues/phemex/phemex_venue.hpp>
#include <raikiri/venues/toobit/toobit_venue.hpp>
#include <raikiri/venues/upbit/upbit_venue.hpp>
#include <raikiri/venues/weex/weex_venue.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace raikiri::exec {
namespace {

constexpr std::uint64_t k_ms = 1'000'000;
constexpr std::uint64_t k_s = 1'000'000'000;

class NullSink final : public GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent&) noexcept override {}

    void on_connectivity(const ConnEvent&) noexcept override {}
};

std::string fixture(std::string_view name) {
    std::ifstream in{std::string{RAIKIRI_SERVER_TIME_FIXTURES} + "/" + std::string{name} + ".json"};
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

struct Case {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Perp};
    std::string_view fixture;
    std::string_view host;
    std::string_view target;
    std::uint64_t server_ns{0};
    std::uint64_t resolution_ns{k_ms};
    bool signs{true};
};

class VenueClock : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        venues::apex::register_adapter();
        venues::aster::register_adapter();
        venues::backpack::register_adapter();
        venues::binance::register_adapter();
        venues::bingx::register_adapter();
        venues::bitfinex::register_adapter();
        venues::bitget::register_adapter();
        venues::bitunix::register_adapter();
        venues::blofin::register_adapter();
        venues::bybit::register_adapter();
        venues::coinbase::register_adapter();
        venues::cryptocom::register_adapter();
        venues::deribit::register_adapter();
        venues::derive::register_adapter();
        venues::dydx::register_adapter();
        venues::edgex::register_adapter();
        venues::extended::register_adapter();
        venues::gate::register_adapter();
        venues::grvt::register_adapter();
        venues::htx::register_adapter();
        venues::hyperliquid::register_adapter();
        venues::kraken::register_adapter();
        venues::kucoin::register_adapter();
        venues::lbank::register_adapter();
        venues::lighter::register_adapter();
        venues::mexc::register_adapter();
        venues::okx::register_adapter();
        venues::pacifica::register_adapter();
        venues::phemex::register_adapter();
        venues::toobit::register_adapter();
        venues::weex::register_adapter();
        venues::xt::register_adapter();
        venues::bitstamp::register_adapter();
        venues::bithumb::register_adapter();
        venues::upbit::register_adapter();
    }

    NullSink sink;
    LiveClock clock;
};

TEST_F(VenueClock, EachVenueReadsItsOwnLiveAnswer) {
    const Case cases[] = {
        {VenueId::Binance, MarketType::Perp, "binance_perp", "fapi.binance.com", "/fapi/v1/time", 1791126450782 * k_ms},
        {VenueId::Binance, MarketType::Spot, "binance_spot", "api.binance.com", "/api/v3/time", 1791126451216 * k_ms},
        {VenueId::Aster, MarketType::Perp, "aster", "fapi.asterdex.com", "/fapi/v1/time", 1791126451632 * k_ms},
        {VenueId::Bybit, MarketType::Perp, "bybit", "api.bybit.com", "/v5/market/time", 1791126452028911755, 1'000},
        {VenueId::Okx, MarketType::Perp, "okx", "www.okx.com", "/api/v5/public/time", 1791126452445 * k_ms},
        {VenueId::Gate, MarketType::Perp, "gate", "api.gateio.ws", "/api/v4/spot/time", 1791126453620 * k_ms},
        {VenueId::Kucoin, MarketType::Spot, "kucoin_spot", "api.kucoin.com", "/api/v1/timestamp", 1791126454126 * k_ms},
        {VenueId::Kucoin,
         MarketType::Perp,
         "kucoin_perp",
         "api-futures.kucoin.com",
         "/api/v1/timestamp",
         1791126454593 * k_ms},
        {VenueId::Bingx,
         MarketType::Perp,
         "bingx_perp",
         "open-api.bingx.com",
         "/openApi/swap/v2/server/time",
         1791126455055 * k_ms},
        {VenueId::Xt, MarketType::Spot, "xt_spot", "sapi.xt.com", "/v4/public/time", 1791126455788 * k_ms},
        {VenueId::Xt,
         MarketType::Perp,
         "xt_perp",
         "fapi.xt.com",
         "/future/market/v1/public/time",
         1791126456157 * k_ms},
        {VenueId::Mexc, MarketType::Spot, "mexc_spot", "api.mexc.com", "/api/v3/time", 1791126456624 * k_ms},
        {VenueId::Mexc,
         MarketType::Perp,
         "mexc_perp",
         "contract.mexc.com",
         "/api/v1/contract/ping",
         1791126457040 * k_ms},
        {VenueId::Deribit,
         MarketType::Perp,
         "deribit",
         "www.deribit.com",
         "/api/v2/public/get_time",
         1791126457349 * k_ms},
        {VenueId::Coinbase, MarketType::Spot, "coinbase", "api.exchange.coinbase.com", "/time", 1791126457771 * k_ms},
        {VenueId::Bitget, MarketType::Perp, "bitget", "api.bitget.com", "/api/v2/public/time", 1791126458411 * k_ms},
        {VenueId::Htx, MarketType::Spot, "htx_spot", "api-aws.huobi.pro", "/v1/common/timestamp", 1791126459046 * k_ms},
        {VenueId::Htx, MarketType::Perp, "htx_perp", "api.hbdm.vn", "/api/v1/timestamp", 1791126459481 * k_ms},
        {VenueId::Weex,
         MarketType::Perp,
         "weex",
         "api-contract.weex.com",
         "/capi/v3/market/time",
         1791126459942 * k_ms},
        {VenueId::Kraken,
         MarketType::Perp,
         "kraken_perp",
         "futures.kraken.com",
         "/derivatives/api/v3/instruments/status",
         1791126514335 * k_ms},
        {VenueId::Phemex,
         MarketType::Perp,
         "phemex",
         "api.phemex.com",
         "/public/time",
         1791126460334 * k_ms,
         k_ms,
         false},
        {VenueId::Backpack,
         MarketType::Perp,
         "backpack",
         "api.backpack.exchange",
         "/api/v1/time",
         1791126460778 * k_ms,
         k_ms,
         false},
        {VenueId::Toobit,
         MarketType::Perp,
         "toobit",
         "api.toobit.com",
         "/api/v1/time",
         1791126461236 * k_ms,
         k_ms,
         false},
        {VenueId::Apex,
         MarketType::Perp,
         "apex",
         "omni.apex.exchange",
         "/api/v3/time",
         1791126461668 * k_ms,
         k_ms,
         false},
        {VenueId::Dydx, MarketType::Perp, "dydx", "indexer.dydx.trade", "/v4/time", 1791126462580 * k_ms, k_ms, false},
        {VenueId::Edgex,
         MarketType::Perp,
         "edgex_perp",
         "edgex-prod-v2.edgex.exchange",
         "/api/v2/public/meta/getServerTime",
         1791126505574 * k_ms,
         k_ms,
         false},
        {VenueId::Derive,
         MarketType::Perp,
         "derive",
         "api.lyra.finance",
         "/public/get_time",
         1791126463894 * k_ms,
         k_ms,
         false},
        {VenueId::Lbank,
         MarketType::Spot,
         "lbank_spot",
         "api.lbank.info",
         "/v2/timestamp.do",
         1791126464334 * k_ms,
         k_ms,
         false},
        {VenueId::Lbank,
         MarketType::Perp,
         "lbank_perp",
         "lbkperp.lbank.com",
         "/cfd/openApi/v1/pub/getTime",
         1791126509701 * k_ms,
         k_ms,
         false},
        {VenueId::Lighter,
         MarketType::Perp,
         "lighter",
         "mainnet.zklighter.elliot.ai",
         "/",
         1791126507 * k_s,
         k_s,
         false},
        {VenueId::Grvt, MarketType::Perp, "grvt", "market-data.grvt.io", "/time", 1791299515346 * k_ms, k_ms, false},
        {VenueId::Hyperliquid,
         MarketType::Perp,
         "hyperliquid",
         "api.hyperliquid.xyz",
         "/info",
         1791299515557 * k_ms,
         k_ms,
         false},
        {VenueId::Blofin,
         MarketType::Perp,
         "blofin",
         "openapi.blofin.com",
         "/api/v1/market/books?instId=BTC-USDT&size=1",
         1791299516004 * k_ms,
         k_ms,
         false},
        {VenueId::CryptoCom,
         MarketType::Perp,
         "cryptocom",
         "api.crypto.com",
         "/exchange/v1/public/get-book?instrument_name=BTCUSD-PERP&depth=1",
         1791299516343 * k_ms,
         k_ms,
         false},
        {VenueId::Upbit,
         MarketType::Spot,
         "upbit",
         "api.upbit.com",
         "/v1/orderbook?markets=KRW-BTC&count=1",
         1791306147866 * k_ms,
         k_ms,
         false},
        {VenueId::Bithumb,
         MarketType::Spot,
         "bithumb",
         "api.bithumb.com",
         "/v1/orderbook?markets=KRW-BTC&count=1",
         1791306148868 * k_ms,
         k_ms,
         false},
        {VenueId::Bitstamp,
         MarketType::Perp,
         "bitstamp",
         "www.bitstamp.net",
         "/api/v2/ticker/btcusd/",
         1791306146 * k_s,
         k_s,
         false},
    };
    for (const auto& c : cases) {
        const auto label = std::string{c.fixture};
        auto adapter = make_venue({.venue = c.venue, .market = c.market}, sink, clock);
        ASSERT_TRUE(adapter.has_value()) << label;
        ASSERT_TRUE(adapter->clock.has_value()) << label;
        const auto& query = adapter->clock->query();
        EXPECT_EQ(query.host, c.host) << label;
        EXPECT_EQ(query.target, c.target) << label;
        EXPECT_EQ(query.resolution_ns, c.resolution_ns) << label;
        EXPECT_EQ(adapter->clock->signs_with_offset(), c.signs) << label;
        ASSERT_NE(query.parse, nullptr) << label;
        EXPECT_EQ(query.parse(fixture(c.fixture)), c.server_ns) << label;
        EXPECT_FALSE(query.parse(R"({"code":50001,"msg":"busy"})").has_value()) << label;
    }
}

TEST_F(VenueClock, DeriveAsksByPost) {
    auto adapter = make_venue({.venue = VenueId::Derive, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value() && adapter->clock.has_value());
    EXPECT_EQ(adapter->clock->query().body, "{}");
}

TEST_F(VenueClock, HyperliquidAsksForTheExchangeStatusByPost) {
    auto adapter = make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value() && adapter->clock.has_value());
    EXPECT_EQ(adapter->clock->query().body, R"({"type":"exchangeStatus"})");
}

TEST_F(VenueClock, VenuesWithoutAServerTimeMethodSayNothing) {
    for (const auto id : {VenueId::Extended, VenueId::Pacifica, VenueId::Bitunix, VenueId::Bitfinex}) {
        const auto market = caps_of(id).markets.perp ? MarketType::Perp : MarketType::Spot;
        auto adapter = make_venue({.venue = id, .market = market}, sink, clock);
        ASSERT_TRUE(adapter.has_value()) << to_string(id);
        EXPECT_FALSE(adapter->clock.has_value()) << to_string(id);
    }
}

TEST(JsonTime, ReadsNumbersSecondsAndBareBodies) {
    EXPECT_EQ(json_number(R"({"a":{"time":5},"timeCost":3})", "time"), 5U);
    EXPECT_EQ(json_number(R"({"ts":"17"})", "ts", 2), 34U);
    EXPECT_EQ(json_number("1791126460778", ""), 1791126460778U);
    EXPECT_FALSE(json_number(R"({"timeCost":3})", "time").has_value());
    EXPECT_FALSE(json_number(R"({"time":null})", "time").has_value());
    EXPECT_EQ(json_seconds(R"({"epoch":1791126462.58})", "epoch"), 1791126462580000000U);
    EXPECT_EQ(json_seconds(R"({"epoch":12})", "epoch"), 12 * k_s);
    EXPECT_EQ(json_seconds(R"({"epoch":1.1234567891})", "epoch"), 1'123'456'789U);
    EXPECT_EQ(ms_to_ns(std::optional<std::uint64_t>{3}), 3 * k_ms);
    EXPECT_FALSE(ms_to_ns(std::optional<std::uint64_t>{0}).has_value());
}

} // namespace
} // namespace raikiri::exec
