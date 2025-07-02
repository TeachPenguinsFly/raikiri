#include <raikiri/admin/history.hpp>

#include <glaze/glaze.hpp>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace raikiri::admin {
namespace {

// The same options routes.cpp writes with: a field with no answer is absent,
// never null and never zero.
constexpr glz::opts k_opts{.skip_null_members = true};

template <typename T> std::string json_of(const T& value) {
    std::string out;
    EXPECT_FALSE(glz::write<k_opts>(value, out));
    return out;
}

std::string hex_of(std::string_view raw) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string out;
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(alphabet[byte >> 4U]);
        out.push_back(alphabet[byte & 0x0FU]);
    }
    return out;
}

// ------------------------------------------------------------- cursors ----

TEST(AdminHistoryCursor, ACursorRoundTripsAndTellsTheClientNothing) {
    const Cursor cursor{.ts = 1'787'840'965'107'000'000ULL, .key = "bybit", .key2 = "1699:9021"};
    const auto text = cursor.encode();

    // Opaque by contract. A client that could read one would start building
    // one, and the day the ordering key changes its pages would move silently.
    EXPECT_EQ(text.find("bybit"), std::string::npos) << text;
    EXPECT_EQ(text.find("1787840965107000000"), std::string::npos) << text;

    const auto back = Cursor::decode(text);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, cursor);
}

TEST(AdminHistoryCursor, AnEmptySecondKeyIsStillOneCursor) {
    const Cursor cursor{.ts = 42, .key = "fee:bybit:88121", .key2 = {}};
    const auto back = Cursor::decode(cursor.encode());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, cursor);
    EXPECT_TRUE(back->key2.empty());
}

TEST(AdminHistoryCursor, AForgedCursorIsRefusedRatherThanHalfRead) {
    constexpr char sep = '\x1F';
    EXPECT_FALSE(Cursor::decode("").has_value()) << "no separators at all";
    EXPECT_FALSE(Cursor::decode("abc").has_value()) << "odd number of hex digits";
    EXPECT_FALSE(Cursor::decode("zzzz").has_value()) << "not hex";
    EXPECT_FALSE(Cursor::decode(hex_of(std::string{"42"} + sep + "one-separator")).has_value());
    EXPECT_FALSE(Cursor::decode(hex_of(std::string{"later"} + sep + "a" + sep + "b")).has_value())
        << "a ts that is not digits";
    const auto four = std::string{"42"} + sep + "a" + sep + "b" + sep + "c";
    EXPECT_FALSE(Cursor::decode(hex_of(four)).has_value()) << "one field too many";
}

TEST(AdminHistoryCursor, ACursorNamesTheRowItStoppedOnAndNotAPositionInAList) {
    // Keyset, not offset. Every cursor below is built from the row that was
    // emitted, so the next page resumes strictly after it however many rows
    // arrived meanwhile.
    Order order{};
    order.client_order_id = "281474976710658";
    order.ts_created = "1787840965107000000";
    const auto after_order = cursor_after(order);
    EXPECT_EQ(after_order.ts, 1'787'840'965'107'000'000ULL);
    EXPECT_EQ(after_order.key, "281474976710658");

    Fill fill{};
    fill.venue = "bybit";
    fill.exec_id = "e-9921";
    fill.ts_event = "1787840965107000001";
    const auto after_fill = cursor_after(fill);
    EXPECT_EQ(after_fill.ts, 1'787'840'965'107'000'001ULL);
    EXPECT_EQ(after_fill.key, "bybit");
    EXPECT_EQ(after_fill.key2, "e-9921") << "(venue, exec_id) is what dedup keys on";

    PnlIncrement increment{};
    increment.source_id = "fee:bybit:e-9921";
    increment.ts_event = "1787840965107000002";
    EXPECT_EQ(cursor_after(increment).key, "fee:bybit:e-9921");

    FundingObservation funding{};
    funding.instrument_id = "720575940379279361";
    funding.ts_event = "1787840965107000003";
    EXPECT_EQ(cursor_after(funding).key, "720575940379279361");

    ReconBreak recon{};
    recon.venue = "okx";
    recon.symbol = "BTC-USDT-SWAP";
    recon.ts_seen = "1787840965107000004";
    EXPECT_EQ(cursor_after(recon).key, "okx");
    EXPECT_EQ(cursor_after(recon).key2, "BTC-USDT-SWAP");
}

TEST(AdminHistoryCursor, EveryCursorSurvivesItsOwnEncoding) {
    Fill fill{};
    fill.venue = "deribit";
    fill.exec_id = "ETH-28AUG26-3000-C-11";
    fill.ts_event = "1787840965107000000";
    const auto back = Cursor::decode(cursor_after(fill).encode());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, cursor_after(fill));
}

// --------------------------------------------------------------- units ----

TEST(AdminHistoryUnits, ATickCountIsNotAPriceUntilAnInstrumentSaysWhatATickIs) {
    // 79418.30 at a 0.1 tick is 794183 ticks, and 2.5 at a 0.001 lot is 2500
    // lots. Emitting either count would be wrong by four orders of magnitude.
    const Scales scales{.tick_size = *parse_decimal_e9("0.1"),
                        .lot_size = *parse_decimal_e9("0.001")};
    EXPECT_EQ(scales.price(PriceTicks::from_raw(794'183)), "79418.3");
    EXPECT_EQ(scales.size(QtyLots::from_raw(2'500)), "2.5");
    EXPECT_EQ(scales.size(QtyLots::from_raw(-2'500)), "-2.5") << "a short is signed, not absent";
}

TEST(AdminHistoryUnits, AnInstrumentTheCatalogueDoesNotHoldRendersNothingAtAll) {
    // raikiri.instruments is loaded by ops tooling while persisterd writes
    // orders regardless, so this is a state the API really reaches.
    const Scales unknown{};
    EXPECT_FALSE(unknown.price(PriceTicks::from_raw(794'183)).has_value());
    EXPECT_FALSE(unknown.size(QtyLots::from_raw(2'500)).has_value());

    const Scales half{.tick_size = *parse_decimal_e9("0.1"), .lot_size = FixedE9::undef()};
    EXPECT_TRUE(half.price(PriceTicks::from_raw(794'183)).has_value());
    EXPECT_FALSE(half.size(QtyLots::from_raw(2'500)).has_value());
}

TEST(AdminHistoryUnits, AnAbsentValueIsAbsentAndAZeroIsAZero) {
    const Scales scales{.tick_size = *parse_decimal_e9("0.1"),
                        .lot_size = *parse_decimal_e9("0.001")};
    EXPECT_FALSE(scales.price(PriceTicks::undef()).has_value()) << "a market order carries none";
    EXPECT_FALSE(scales.size(QtyLots::undef()).has_value());
    EXPECT_EQ(scales.size(QtyLots::from_raw(0)), "0") << "flat is a quantity, not a hole";
}

TEST(AdminHistoryUnits, ACatalogueRowWithAnImpossibleScaleRefusesRatherThanAborting) {
    // The domain's arithmetic aborts on overflow, which is right for numbers
    // this system computed and wrong for a row it merely read: a bad catalogue
    // entry must not take the admin API down with it.
    const Scales absurd{.tick_size = FixedE9::from_raw(1'000'000'000'000'000'000),
                        .lot_size = FixedE9::from_raw(1'000'000'000'000'000'000)};
    EXPECT_FALSE(absurd.price(PriceTicks::from_raw(1'000'000'000)).has_value());
    EXPECT_FALSE(absurd.size(QtyLots::from_raw(-1'000'000'000)).has_value());

    const Scales negative{.tick_size = FixedE9::from_raw(-1), .lot_size = FixedE9::from_raw(0)};
    EXPECT_FALSE(negative.price(PriceTicks::from_raw(1)).has_value());
    EXPECT_FALSE(negative.size(QtyLots::from_raw(1)).has_value());
}

// ---------------------------------------------------------------- json ----

TEST(AdminHistoryShape, EveryNumberTooLargeForADoubleLeavesAsAString) {
    Order order{};
    order.client_order_id = "281474976710658";
    order.instrument_id = "720575940379279361";
    order.side = "buy";
    order.order_type = "limit";
    order.tif = "gtc";
    order.price = "79418.3";
    order.qty = "0.5";
    order.status = "accepted";
    order.ts_created = "1787840965107000000";
    order.ts_updated = "1787840965207000000";

    const auto body = json_of(order);
    EXPECT_NE(body.find(R"("client_order_id":"281474976710658")"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("instrument_id":"720575940379279361")"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("ts_created":"1787840965107000000")"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("price":"79418.3")"), std::string::npos) << body;
    // strategy_id is small enough to be a number honestly, and is one.
    EXPECT_NE(body.find(R"("strategy_id":0)"), std::string::npos) << body;
}

TEST(AdminHistoryShape, AFieldWithNoAnswerIsAbsentRatherThanNullOrZero) {
    Order order{};
    order.client_order_id = "281474976710658";
    order.instrument_id = "720575940379279361";
    order.side = "sell";
    order.order_type = "market";
    order.tif = "ioc";
    order.qty = "0.5";
    order.status = "filled";
    order.ts_created = "1787840965107000000";
    order.ts_updated = "1787840965207000000";

    const auto body = json_of(order);
    EXPECT_EQ(body.find("price"), std::string::npos) << "a market order has no price: " << body;
    EXPECT_EQ(body.find("exchange_order_id"), std::string::npos) << body;
    EXPECT_EQ(body.find("leaves"), std::string::npos) << body;
    EXPECT_EQ(body.find("null"), std::string::npos) << body;
}

TEST(AdminHistoryShape, AFundingObservationCarriesOnlyWhatTheVenuePublished) {
    // Few venues publish a predicted rate and none publish all of these. An
    // interval of zero in particular would be read as data: annualising by it
    // divides.
    FundingObservation observation{};
    observation.instrument_id = "720575940379279361";
    observation.rate = "-0.000058784";
    observation.ts_event = "1787840965107000000";

    const auto body = json_of(observation);
    EXPECT_EQ(body.find("predicted_rate"), std::string::npos) << body;
    EXPECT_EQ(body.find("mark_price"), std::string::npos) << body;
    EXPECT_EQ(body.find("interval_seconds"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("rate":"-0.000058784")"), std::string::npos) << body;

    observation.interval_seconds = 3600;
    EXPECT_NE(json_of(observation).find(R"("interval_seconds":3600)"), std::string::npos);
}

TEST(AdminHistoryShape, TheLastPageSaysSoByOmittingItsCursor) {
    OrderPage page{};
    page.items.emplace_back();
    EXPECT_EQ(json_of(page).find("next_cursor"), std::string::npos);

    page.next_cursor = "3438";
    EXPECT_NE(json_of(page).find(R"("next_cursor":"3438")"), std::string::npos);
}

TEST(AdminHistoryShape, AReconBreakSaysWhichKindOfDisagreementItWas) {
    // `unknown` is the venue not answering, and it is NOT agreement: agreeing
    // instruments are never recorded at all.
    ReconBreak recon{};
    recon.venue = "okx";
    recon.symbol = "BTC-USDT-SWAP";
    recon.verdict = "unknown";
    recon.detail = "venue could not report";
    recon.ts_seen = "1787840965107000000";

    const auto body = json_of(recon);
    EXPECT_NE(body.find(R"("verdict":"unknown")"), std::string::npos) << body;
    EXPECT_EQ(body.find("instrument_id"), std::string::npos)
        << "one symbol can be listed in two markets, and naming one is a guess: " << body;
    EXPECT_EQ(body.find("local_qty"), std::string::npos) << body;
}

TEST(AdminHistoryKinds, APnlKindIsAClosedSetAndAnythingElseIsRefused) {
    EXPECT_EQ(pnl_kind_name(PnlKind::Realized), "realized");
    EXPECT_EQ(pnl_kind_name(PnlKind::Fee), "fee");
    EXPECT_EQ(pnl_kind_name(PnlKind::Funding), "funding");
    EXPECT_EQ(pnl_kind_from_string("funding"), PnlKind::Funding);
    EXPECT_FALSE(pnl_kind_from_string("unrealized").has_value());
    EXPECT_FALSE(pnl_kind_from_string("").has_value());
}

// ----------------------------------------------------------- postgres ----

TEST(AdminHistoryDown, ADatabaseThatIsNotThereIsAnEmptyAnswerAndNotAThrow) {
    // A terminal that can show live state without history is more useful than
    // one that refuses to answer at all.
    History history{"host=127.0.0.1 port=1 dbname=nothing connect_timeout=1"};
    EXPECT_FALSE(history.connected());
    EXPECT_FALSE(history.health().reachable);
    EXPECT_TRUE(history.orders({}, std::nullopt).items.empty());
    EXPECT_FALSE(history.order(ClientOrderId::make(1, 2)).has_value());
    EXPECT_TRUE(history.order_events(ClientOrderId::make(1, 2)).empty());
    EXPECT_TRUE(history.fills({}).items.empty());
    EXPECT_TRUE(history.positions(std::nullopt, std::nullopt).empty());
    EXPECT_TRUE(history.pnl({}, std::nullopt).items.empty());
    EXPECT_TRUE(history.funding({}).items.empty());
    EXPECT_TRUE(history.recon_breaks({}).items.empty());
}

// Against a live Postgres — skipped unless RAIKIRI_PG_DSN is set (CI provides
// a service container; locally: infra postgres or docker). Every read swallows
// its own failure by design, so the assertion is not the rows: it is that the
// connection is still open afterwards, which only holds if the statement
// actually parsed and ran.
TEST(AdminHistoryPg, EveryQueryTheContractNamesIsValidSqlAgainstTheRealSchema) {
    const char* dsn = std::getenv("RAIKIRI_PG_DSN");
    if (dsn == nullptr || *dsn == '\0') {
        GTEST_SKIP() << "RAIKIRI_PG_DSN not set";
    }
    History history{dsn};
    ASSERT_TRUE(history.connected());
    if (!history.health().reachable) {
        GTEST_SKIP() << "sql/migrations not applied to $RAIKIRI_PG_DSN";
    }

    const auto survived = [&](std::string_view what) {
        EXPECT_TRUE(history.connected()) << what << " did not survive its own query";
    };

    const AccountId account{.venue = VenueId::Bybit, .market = MarketType::Perp};
    const auto instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7});
    const Cursor numeric{.ts = 1'787'840'965'107'000'000ULL, .key = "281474976710658", .key2 = {}};
    const Cursor textual{.ts = 1'787'840'965'107'000'000ULL, .key = "bybit", .key2 = "e-1"};

    const PageQuery full{.account = account,
                         .instrument = instrument,
                         .since = UnixNanos{1},
                         .cursor = numeric,
                         .limit = 5};

    (void)history.orders({}, std::nullopt);
    survived("unfiltered orders");
    (void)history.orders(full, true);
    survived("open orders");
    (void)history.orders(full, false);
    survived("closed orders");
    (void)history.order(ClientOrderId::make(1, 2));
    survived("one order");
    (void)history.order_events(ClientOrderId::make(1, 2));
    survived("order events");

    (void)history.fills({.account = account,
                         .instrument = instrument,
                         .since = UnixNanos{1},
                         .cursor = textual,
                         .limit = 5});
    survived("fills");

    (void)history.positions(account, std::uint16_t{7});
    survived("positions");
    (void)history.positions(std::nullopt, std::nullopt);
    survived("unfiltered positions");

    (void)history.pnl({.account = account,
                       .since = UnixNanos{1},
                       .cursor = Cursor{.ts = 1, .key = "fee:bybit:e-1", .key2 = {}},
                       .limit = 5},
                      PnlKind::Fee);
    survived("pnl");

    (void)history.funding(full);
    survived("funding");

    (void)history.recon_breaks(
        {.account = account, .since = UnixNanos{1}, .cursor = textual, .limit = 5});
    survived("recon breaks");

    // A cursor whose key is not the number that column holds is answered with
    // nothing, never with a page from somewhere else.
    const PageQuery forged{.cursor = Cursor{.ts = 1, .key = "not-a-number", .key2 = {}}};
    EXPECT_TRUE(history.orders(forged, std::nullopt).items.empty());
    EXPECT_TRUE(history.funding(forged).items.empty());
    survived("a forged cursor");
}

} // namespace
} // namespace raikiri::admin
