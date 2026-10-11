#include <raikiri/reference/gateways.hpp>
#include <raikiri/reference/routes.hpp>
#include <raikiri/reference/segment.hpp>
#include <raikiri/shm/records.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <string>

namespace raikiri::reference {
namespace {

constexpr std::uint64_t k_wall = 1'800'000'000ULL * 1'000'000'000ULL;
constexpr std::uint64_t k_mono = 5'000'000'000ULL;

std::string scratch_dir(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / ("raikiri-reftest-" + std::to_string(::getpid()) + "-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

Row btc_row() {
    Reference ref{.mid = FixedE9::from_raw(100'010'000'000'000),
                  .spot_mid = FixedE9::from_raw(100'005'000'000'000),
                  .perp_mid = FixedE9::from_raw(100'060'000'000'000),
                  .basis_e9 = FixedE9::from_raw(500'000),
                  .conf = FixedE9::from_raw(10'001'000'000),
                  .dispersion_e9 = FixedE9::from_raw(120'000),
                  .quotes = 9,
                  .used_spot = 4,
                  .used_perp = 3,
                  .oldest_age_ns = 1'500'000'000,
                  .refused = false};
    ref.cuts[static_cast<std::size_t>(Cut::Outlier)] = 1;
    ref.cuts[static_cast<std::size_t>(Cut::Stale)] = 1;
    ref.rates.usdt_usd = FixedE9::from_raw(999'800'000);
    ref.rates.usdt_fallback = false;
    return {.asset = "crypto:BTC", .ref = ref};
}

TEST(ReferenceSegment, RowsAndMetricsRoundTripAndKeepTheirSlot) {
    const auto dir = scratch_dir("roundtrip");
    const auto path = dir + "/ref.mid";
    auto writer = RefWriter::create(path, 7, k_mono, 500'000'000);
    ASSERT_TRUE(writer.has_value());
    (*writer)->set_gateways(5, 1);

    const std::vector<Row> rows{btc_row(), {.asset = "crypto:ETH", .ref = Reference{}}};
    (*writer)->publish(rows, {.at_ns = k_wall, .took_ns = 42'000, .quotes = 12, .refused = 1, .rates = {}});

    auto reader = RefReader::attach(path, "reftest");
    ASSERT_TRUE(reader.has_value());
    EXPECT_TRUE(reader->listed());
    auto snap = reader->read(k_mono);
    EXPECT_TRUE(snap.owner_alive);
    ASSERT_EQ(snap.rows.size(), 2U);
    const auto& btc = snap.rows[0];
    EXPECT_EQ(btc.asset_key(), "crypto:BTC");
    EXPECT_EQ(btc.mid.raw, 100'010'000'000'000);
    EXPECT_EQ(btc.perp_mid.raw, 100'060'000'000'000);
    EXPECT_EQ(btc.used_spot, 4);
    EXPECT_EQ(btc.used_perp, 3);
    EXPECT_EQ(btc.cuts[static_cast<std::size_t>(Cut::Outlier)], 1);
    EXPECT_EQ(btc.usdt_usd.raw, 999'800'000);
    EXPECT_EQ(btc.flags, std::uint32_t{FlagUsdcFallback});
    EXPECT_EQ(btc.ts_ns, k_wall);
    EXPECT_EQ(snap.rows[1].asset_key(), "crypto:ETH");
    EXPECT_NE(snap.rows[1].flags & FlagRefused, 0U);
    EXPECT_EQ(snap.metrics.gateways, 5U);
    EXPECT_EQ(snap.metrics.unlisted, 1U);
    EXPECT_EQ(snap.metrics.cycles, 1U);
    EXPECT_EQ(snap.metrics.took_ns, 42'000U);

    // a later cycle in another order writes each asset into the slot it already holds
    const std::vector<Row> swapped{
        {.asset = "crypto:SOL", .ref = Reference{}}, {.asset = "crypto:ETH", .ref = Reference{}}, btc_row()};
    (*writer)->publish(swapped, {.at_ns = k_wall + 1, .rates = {}});
    snap = reader->read(k_mono);
    ASSERT_EQ(snap.rows.size(), 3U);
    EXPECT_EQ(snap.rows[0].asset_key(), "crypto:BTC");
    EXPECT_EQ(snap.rows[2].asset_key(), "crypto:SOL");
    EXPECT_EQ(snap.metrics.cycles, 2U);

    // a gateway's tag does not open the reference segment
    const auto wrong = shm::SegmentClient::attach(path);
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error(), shm::SegError::BadMagic);

    // a second refd on the same path is refused while the first beats
    (*writer)->beat(k_mono);
    EXPECT_FALSE(RefWriter::create(path, 8, k_mono, 500'000'000).has_value());

    reader = std::unexpected{shm::SegError::NotReady};
    writer = std::unexpected{shm::SegError::NotReady};
    std::filesystem::remove_all(dir);
}

TEST(ReferenceSegment, TheRouteServesTheSegmentAndAnswers503WithoutOne) {
    const auto dir = scratch_dir("route");
    const auto path = dir + "/ref.mid";
    ShmReferenceFeed feed{path, "reftest", [] { return k_mono; }};
    const ReferenceRoute route{feed, [] { return k_wall + 250'000'000; }};

    EXPECT_FALSE(route({.method = "GET", .path = "/v1/other"}).has_value());
    auto answer = route({.method = "GET", .path = k_reference_path});
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 503);

    auto writer = RefWriter::create(path, 7, k_mono, 500'000'000);
    ASSERT_TRUE(writer.has_value());
    const std::vector<Row> rows{btc_row(), {.asset = "crypto:ETH", .ref = Reference{}}};
    (*writer)->publish(rows, {.at_ns = k_wall, .rates = {}});

    answer = route({.method = "GET", .path = k_reference_path});
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 200);
    const auto& body = answer->body;
    EXPECT_NE(body.find(R"("asset":"crypto:BTC")"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("mid":"100010)"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("basis_bp":"5.00")"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("conf_bp":"1.00")"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("age_ms":250)"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("outlier":1)"), std::string::npos) << body;

    answer = route({.method = "GET", .path = k_reference_path, .query = "asset=ETH"});
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->body.find("crypto:BTC"), std::string::npos);
    EXPECT_NE(answer->body.find("crypto:ETH"), std::string::npos);

    answer = route({.method = "POST", .path = k_reference_path});
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 405);
    writer = std::unexpected{shm::SegError::NotReady};
    std::filesystem::remove_all(dir);
}

TEST(ReferenceSegment, BasisPointsPrintWithTwoDecimals) {
    EXPECT_EQ(bp_text(1'234'567), "12.34");
    EXPECT_EQ(bp_text(-500'000), "-5.00");
    EXPECT_EQ(bp_text(0), "0.00");
}

TEST(ReferenceGateways, AGatewaySegmentIsReadIntoQuotesByAssetKey) {
    const auto dir = scratch_dir("gateways");
    auto owner = shm::SegmentOwner::create(dir + "/gw.kraken-spot", shm::k_gateway_segment_bytes, 3, k_mono);
    ASSERT_TRUE(owner.has_value());
    auto channels = shm::create_gateway_channels(*owner);
    ASSERT_TRUE(channels.has_value());

    SpotInstrument usdt{};
    usdt.base.id = InstrumentId::make(VenueId::Kraken, MarketType::Spot, SymbolId{0});
    usdt.base.local_symbol = *SymbolText::from("USDT/USD");
    usdt.base.exchange_symbol = *SymbolText::from("USDTUSD");
    usdt.base.asset_class = AssetClass::Crypto;
    usdt.base.underlying = *UnderlyingText::from("USDT");
    usdt.base.tick_size = FixedE9::from_raw(100'000);
    usdt.base.lot_size = FixedE9::from_raw(1'000'000);
    ASSERT_TRUE(channels->catalogue->push(usdt).has_value());
    SpotInstrument nameless = usdt;
    nameless.base.id = InstrumentId::make(VenueId::Kraken, MarketType::Spot, SymbolId{1});
    nameless.base.asset_class = AssetClass::Unknown;
    ASSERT_TRUE(channels->catalogue->push(nameless).has_value());
    const Bbo bbo{.instrument = usdt.base.id,
                  .bid = PriceTicks::from_raw(9'998),
                  .ask = PriceTicks::from_raw(9'999),
                  .bid_qty = QtyLots::from_raw(5'000),
                  .ask_qty = QtyLots::from_raw(7'000),
                  .ts_event = {},
                  .ts_init = {k_wall}};
    channels->bbo->write(0, bbo);
    channels->bbo->write(1, bbo);
    channels->ticker_stats->write(0, {.instrument = usdt.base.id, .volume_24h_quote = FixedE6::from_raw(2'000'000)});
    owner->publish();

    std::uint64_t mono = k_mono;
    GatewayQuotes gateways{dir, "reftest", [&mono] { return mono; }};
    const auto found = gateways.rescan();
    ASSERT_EQ(found.attached.size(), 1U);
    EXPECT_EQ(gateways.attached(), 1U);
    EXPECT_EQ(gateways.unlisted(), 0U);

    owner->beat(mono);
    auto book = gateways.snapshot();
    ASSERT_EQ(book.size(), 1U);
    EXPECT_EQ(book[0].asset, "crypto:USDT");
    ASSERT_EQ(book[0].quotes.size(), 1U);
    const auto& q = book[0].quotes[0];
    EXPECT_EQ(q.venue, VenueId::Kraken);
    EXPECT_EQ(q.ccy, QuoteCcy::Usd);
    EXPECT_EQ(q.bid.raw, 999'800'000);
    EXPECT_EQ(q.ask.raw, 999'900'000);
    EXPECT_EQ(q.bid_qty.raw, 5'000'000'000);
    EXPECT_EQ(q.ts_ns, k_wall);
    EXPECT_EQ(q.volume_24h.raw, 2'000'000);

    // a gateway that stopped beating contributes nothing
    mono += 10'000'000'000ULL;
    EXPECT_TRUE(gateways.snapshot().empty());
    std::filesystem::remove_all(dir);
}

} // namespace
} // namespace raikiri::reference
