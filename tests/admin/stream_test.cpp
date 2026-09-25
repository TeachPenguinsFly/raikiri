#include <raikiri/admin/format.hpp>
#include <raikiri/admin/stream.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace raikiri::admin {
namespace {

// past 2^53
constexpr std::uint64_t k_big_id = 4'611'686'018'427'387'905ULL;
constexpr UnixNanos k_ts{1'787'840'965'107'000'000ULL};

InstrumentBase linear_base() {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{3});
    base.exchange_symbol = *SymbolText::from("BTC-PERPETUAL");
    base.local_symbol = *SymbolText::from("BTC/USD");
    base.tick_size = *parse_decimal_e9("0.01");
    base.lot_size = *parse_decimal_e9("0.001");
    base.min_notional = *parse_decimal_e9("1");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return base;
}

Bbo two_sided(const InstrumentBase& base) {
    Bbo bbo{};
    bbo.instrument = base.id;
    bbo.bid = *base.price_to_ticks(*parse_decimal_e9("79418.30"));
    bbo.bid_qty = *base.qty_to_lots(*parse_decimal_e9("1.5"));
    bbo.ask = *base.price_to_ticks(*parse_decimal_e9("79418.40"));
    bbo.ask_qty = *base.qty_to_lots(*parse_decimal_e9("2"));
    bbo.ts_event = k_ts;
    return bbo;
}

TEST(AdminStream, ASubscribeIsAChannelAHandleAndItsInstruments) {
    const auto message = parse_client_message(R"({"op":"subscribe","id":"q1","quotes":["123","4611686018427387905"]})");
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->op, Op::Subscribe);
    EXPECT_EQ(message->id, "q1");
    ASSERT_EQ(message->quotes.size(), 2U);
    EXPECT_EQ(message->quotes[0].packed, 123U);
    EXPECT_EQ(message->quotes[1].packed, k_big_id);
    EXPECT_FALSE(message->millihertz.has_value());

    const auto book = parse_client_message(R"({"op":"subscribe","id":"b1","book":"123"})");
    ASSERT_TRUE(book.has_value());
    ASSERT_TRUE(book->book.has_value());
    EXPECT_EQ(book->book->packed, 123U);
}

TEST(AdminStream, AnInstrumentIdThatIsNotDigitsIsRefusedRatherThanTruncated) {
    for (const auto* text : {R"({"op":"subscribe","id":"q1","quotes":["79418.30"]})",
                             R"({"op":"subscribe","id":"q1","quotes":["abc"]})",
                             R"({"op":"subscribe","id":"q1","quotes":["-1"]})",
                             R"({"op":"subscribe","id":"q1","quotes":["99999999999999999999999"]})"}) {
        const auto message = parse_client_message(text);
        ASSERT_FALSE(message.has_value()) << text;
        EXPECT_EQ(message.error().status, 400);
    }
}

TEST(AdminStream, AMalformedMessageIsOneBadFrameAndNotADroppedConnection) {
    const auto message = parse_client_message("not json at all");
    ASSERT_FALSE(message.has_value());
    EXPECT_EQ(message.error().status, 400);

    const auto frame = problem_frame({}, message.error());
    EXPECT_EQ(frame.find(R"("id")"), std::string::npos) << frame;
    EXPECT_TRUE(frame.starts_with(R"({"type":"problem","problem":{)")) << frame;
}

TEST(AdminStream, ASubscribeNamesExactlyOneChannel) {
    const auto none = parse_client_message(R"({"op":"subscribe","id":"x"})");
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().title, "subscribe named no channel");

    const auto both = parse_client_message(R"({"op":"subscribe","id":"x","quotes":["1"],"book":"1"})");
    ASSERT_FALSE(both.has_value());
    EXPECT_EQ(both.error().title, "one channel per subscription");
}

TEST(AdminStream, AFieldThisServerDoesNotKnowIsAClientOneVersionAhead) {
    const auto message = parse_client_message(R"({"op":"subscribe","id":"q1","quotes":["1"],"depth":25})");
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->quotes.size(), 1U);

    const auto bad = parse_client_message(R"({"op":"subscribe","id":"a1","events":["trades"]})");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().title, "unknown event kind");
}

TEST(AdminStream, PositionsAndHealthAreStateEvenThoughTheyArriveThroughEvents) {
    const auto message = parse_client_message(
        R"({"op":"subscribe","id":"a1","events":["orders","fills","positions","health"]})");
    ASSERT_TRUE(message.has_value());
    ASSERT_EQ(message->events.size(), 4U);
    EXPECT_TRUE(sequenced(message->events[0]));
    EXPECT_TRUE(sequenced(message->events[1]));
    EXPECT_FALSE(sequenced(message->events[2]));
    EXPECT_FALSE(sequenced(message->events[3]));
}

TEST(AdminStream, UnsubscribeCarriesOnlyTheHandle) {
    const auto message = parse_client_message(R"({"op":"unsubscribe","id":"q1"})");
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->op, Op::Unsubscribe);
    EXPECT_EQ(message->id, "q1");
    EXPECT_TRUE(message->quotes.empty());

    EXPECT_FALSE(parse_client_message(R"({"op":"resubscribe","id":"q1"})").has_value());
    EXPECT_FALSE(parse_client_message(R"({"op":"unsubscribe","id":""})").has_value());
}

TEST(AdminStream, CadenceIsNegotiatedRatherThanObeyed) {
    EXPECT_EQ(default_millihertz(Feed::Quotes, 1), 30'000);
    EXPECT_EQ(default_millihertz(Feed::Quotes, 40), 10'000);
    EXPECT_EQ(default_millihertz(Feed::Book, 1), 5'000);
    EXPECT_EQ(default_millihertz(Feed::Book, 8), 1'000);
    EXPECT_EQ(default_millihertz(Feed::Greeks, 8), 1'000);
    EXPECT_EQ(default_millihertz(Feed::Positions, 200), 1'000);
    EXPECT_EQ(default_millihertz(Feed::Health, 0), 1'000);

    EXPECT_EQ(negotiate_millihertz(Feed::Quotes, 1, 200'000), k_max_millihertz);
    EXPECT_EQ(negotiate_millihertz(Feed::Greeks, 4, 60'000), k_max_millihertz);
    EXPECT_EQ(negotiate_millihertz(Feed::Quotes, 1, 0), k_min_millihertz);
    EXPECT_EQ(negotiate_millihertz(Feed::Quotes, 1, 5'000), 5'000);
    EXPECT_EQ(negotiate_millihertz(Feed::Quotes, 40, std::nullopt), 10'000);

    EXPECT_EQ(negotiate_millihertz(Feed::Fills, 0, 30'000), 0);
    EXPECT_EQ(period_ns(0), 0U);

    EXPECT_EQ(period_ns(30'000), 33'333'333U);
    EXPECT_EQ(period_ns(1'000), 1'000'000'000U);
    EXPECT_EQ(period_ns(100), 10'000'000'000U);
}

TEST(AdminStream, TheAskedForCadenceNeverPassesThroughADouble) {
    const auto half = parse_client_message(R"({"op":"subscribe","id":"q1","quotes":["1"],"hz":0.5})");
    ASSERT_TRUE(half.has_value());
    EXPECT_EQ(half->millihertz, 500);

    const auto plain = parse_client_message(R"({"op":"subscribe","id":"q1","quotes":["1"],"hz":30})");
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->millihertz, 30'000);

    const auto exponent = parse_client_message(R"({"op":"subscribe","id":"q1","quotes":["1"],"hz":1e2})");
    ASSERT_TRUE(exponent.has_value());
    EXPECT_EQ(exponent->millihertz, 100'000);
    EXPECT_EQ(negotiate_millihertz(Feed::Quotes, 1, exponent->millihertz), k_max_millihertz);

    EXPECT_FALSE(parse_client_message(R"({"op":"subscribe","id":"q1","quotes":["1"],"hz":"fast"})").has_value());
}

TEST(AdminStream, TheAckStatesWhatTheServerWillActuallyDo) {
    EXPECT_EQ(ack_frame("q1", 30'000), R"({"type":"ack","id":"q1","hz":30})");
    EXPECT_EQ(ack_frame("q1", 500), R"({"type":"ack","id":"q1","hz":0.5})");
    EXPECT_EQ(ack_frame("a1", std::nullopt), R"({"type":"ack","id":"a1"})");
    EXPECT_EQ(ack_frame(R"(a"b)", std::nullopt), R"({"type":"ack","id":"a\"b"})");
}

TEST(AdminStream, ACellIsCopiedOnlyOnceItsVersionHasMoved) {
    ChangeGate gate{2};
    EXPECT_FALSE(gate.moved(0, 0)) << "version 0 is a cell never written, not a value";
    EXPECT_TRUE(gate.moved(0, 2));

    EXPECT_TRUE(gate.moved(0, 2));

    gate.accept(0, 2);
    EXPECT_FALSE(gate.moved(0, 2));
    EXPECT_TRUE(gate.moved(0, 4));
    EXPECT_TRUE(gate.moved(1, 2)) << "slots are independent";

    EXPECT_FALSE(gate.moved(9, 2));
    gate.accept(9, 2);
}

TEST(AdminStream, EveryNumberThatCannotSurviveADoubleLeavesAsAString) {
    const auto base = linear_base();
    const auto quote = quote_of(two_sided(base), base);
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->bid, "79418.3");
    EXPECT_EQ(quote->bid_size, "1.5");
    EXPECT_EQ(quote->ask, "79418.4");
    EXPECT_EQ(quote->ts_event, "1787840965107000000");

    const std::vector<Quote> rows{*quote};
    const auto frame = quotes_frame("q1", k_ts, rows);
    EXPECT_EQ(frame,
              R"({"type":"quotes","id":"q1","ts":"1787840965107000000","quotes":[)"
              R"({"instrument_id":")" +
                  digits(base.id) +
                  R"(","bid":"79418.3","bid_size":"1.5","ask":"79418.4","ask_size":"2",)"
                  R"("ts_event":"1787840965107000000"}]})");
}

TEST(AdminStream, AQuoteWithOneSideIsNotAQuoteYet) {
    const auto base = linear_base();
    auto bbo = two_sided(base);
    bbo.ask = PriceTicks::undef();
    EXPECT_FALSE(quote_of(bbo, base).has_value());

    bbo = two_sided(base);
    bbo.bid_qty = QtyLots::undef();
    EXPECT_FALSE(quote_of(bbo, base).has_value());
}

TEST(AdminStream, TheBookIsBestFirstAndOnlyAsDeepAsItWasFilled) {
    const auto base = linear_base();
    BookSnapshot snapshot{};
    snapshot.instrument = base.id;
    snapshot.ts_event = k_ts;
    snapshot.bid_count = 2;
    snapshot.ask_count = 1;
    snapshot.bids[0] = {.price = *base.price_to_ticks(*parse_decimal_e9("79418.30")),
                        .qty = *base.qty_to_lots(*parse_decimal_e9("1.5"))};
    snapshot.bids[1] = {.price = *base.price_to_ticks(*parse_decimal_e9("79418.20")),
                        .qty = *base.qty_to_lots(*parse_decimal_e9("3"))};
    // levels past the count are left over from the previous snapshot
    snapshot.bids[2] = {.price = *base.price_to_ticks(*parse_decimal_e9("1")),
                        .qty = *base.qty_to_lots(*parse_decimal_e9("1"))};
    snapshot.asks[0] = {.price = *base.price_to_ticks(*parse_decimal_e9("79418.40")),
                        .qty = *base.qty_to_lots(*parse_decimal_e9("2"))};

    const auto book = book_of(snapshot, base, k_default_levels);
    ASSERT_EQ(book.bids.size(), 2U);
    ASSERT_EQ(book.asks.size(), 1U);
    EXPECT_EQ(book.bids[0].price, "79418.3");
    EXPECT_EQ(book.bids[1].price, "79418.2");
    EXPECT_EQ(book.asks[0].size, "2");

    const auto frame = book_frame("b1", k_ts, book);
    EXPECT_TRUE(frame.starts_with(R"({"type":"book","id":"b1","ts":"1787840965107000000","book":{)")) << frame;
    EXPECT_NE(frame.find(R"("bids":[{"price":"79418.3","size":"1.5"})"), std::string::npos) << frame;
}

TEST(AdminStream, ADepthVisualiserGetsTheLevelsItDrawsAndNotTheHundredHeld) {
    const auto base = linear_base();
    BookSnapshot snapshot{};
    snapshot.instrument = base.id;
    snapshot.ts_event = k_ts;
    snapshot.bid_count = 40;
    snapshot.ask_count = 40;
    for (std::size_t i = 0; i < 40; ++i) {
        const auto step = std::to_string(70'000 + i);
        snapshot.bids[i] = {.price = *base.price_to_ticks(*parse_decimal_e9(step)),
                            .qty = *base.qty_to_lots(*parse_decimal_e9("1"))};
        snapshot.asks[i] = snapshot.bids[i];
    }

    const auto drawn = book_of(snapshot, base, k_default_levels);
    EXPECT_EQ(drawn.bids.size(), k_default_levels);
    EXPECT_EQ(drawn.asks.size(), k_default_levels);

    EXPECT_EQ(book_of(snapshot, base, 5).bids.size(), 5U);

    EXPECT_EQ(book_of(snapshot, base, 100).bids.size(), 40U);
}

TEST(AdminStream, OneLadderIsSampledFasterThanEightOfThem) {
    EXPECT_EQ(default_millihertz(Feed::Book, 1), 5'000);
    EXPECT_EQ(default_millihertz(Feed::Book, 8), 1'000);
    EXPECT_EQ(negotiate_millihertz(Feed::Book, 1, std::nullopt), 5'000);
    EXPECT_EQ(negotiate_millihertz(Feed::Book, 8, std::nullopt), 1'000);
    EXPECT_EQ(negotiate_millihertz(Feed::Book, 1, 10'000), 10'000);
    EXPECT_EQ(negotiate_millihertz(Feed::Book, 1, 90'000), k_max_millihertz);
}

TEST(AdminStream, FundingSaysItsIntervalOrSaysNothing) {
    const auto base = linear_base();
    FundingUpdate update{};
    update.instrument = base.id;
    update.rate = *parse_decimal_e9("-0.000058784");
    update.predicted_rate = *parse_decimal_e9("0.0001");
    update.mark_price = *base.price_to_ticks(*parse_decimal_e9("79418.30"));
    update.next_funding_time = k_ts;
    update.ts_event = k_ts;
    update.interval_s = 3600;

    auto row = funding_of(update, base);
    EXPECT_EQ(row.rate, "-0.000058784");
    EXPECT_EQ(row.mark_price, "79418.3");
    ASSERT_TRUE(row.interval_seconds.has_value());
    EXPECT_EQ(*row.interval_seconds, 3600);

    update.interval_s = 0;
    row = funding_of(update, base);
    EXPECT_FALSE(row.interval_seconds.has_value());

    const std::vector<Funding> rows{row};
    const auto frame = funding_frame("f1", k_ts, rows);
    EXPECT_EQ(frame.find("interval_seconds"), std::string::npos) << frame;
    EXPECT_EQ(frame.find("accrual"), std::string::npos) << frame;
}

TEST(AdminStream, FundingSaysWhetherItAccruesContinuously) {
    const auto base = linear_base();
    FundingUpdate update{};
    update.instrument = base.id;
    update.rate = *parse_decimal_e9("0.0001");
    update.ts_event = k_ts;
    update.interval_s = 28'800;
    update.accrual = FundingAccrual::Continuous;

    const std::vector<Funding> rows{funding_of(update, base)};
    const auto frame = funding_frame("f1", k_ts, rows);
    EXPECT_NE(frame.find(R"("accrual":"continuous")"), std::string::npos) << frame;
}

TEST(AdminStream, AVenuePositionAgesOutRatherThanDecayingIntoFlatness) {
    const auto base = linear_base();
    shm::PositionCell cell{};
    cell.report.instrument = base.id;
    cell.report.net_qty = *base.qty_to_lots(*parse_decimal_e9("-2.5"));
    cell.report.ts_venue = k_ts;
    cell.ts_pulled = k_ts;

    const auto fresh = position_of(cell, base, k_ts + 1'000'000'000ULL);
    EXPECT_EQ(fresh.qty, "-2.5");
    ASSERT_TRUE(fresh.venue_qty.has_value());
    EXPECT_EQ(*fresh.venue_qty, "-2.5");
    EXPECT_EQ(fresh.ts_updated, "1787840965107000000");

    const auto stale = position_of(cell, base, k_ts + k_position_stale_ns + 1);
    EXPECT_FALSE(stale.venue_qty.has_value());
    EXPECT_FALSE(stale.venue_reported_at.has_value());

    const std::vector<Position> rows{stale};
    const auto frame = positions_frame("p1", k_ts, rows);
    EXPECT_EQ(frame.find("venue_qty"), std::string::npos) << frame;
    EXPECT_NE(frame.find(R"({"type":"positions","id":"p1","ts":")"), std::string::npos) << frame;
}

OrderEvent filled_event(const InstrumentBase& base) {
    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.side = Side::Sell;
    event.liquidity = Liquidity::Maker;
    event.instrument = base.id;
    event.client_order_id = ClientOrderId::make(7, 42);
    event.exchange_order_id = *ExchangeOrderId::from("X-1");
    event.exec_id = *ExecId::from("E-1");
    event.price = *base.price_to_ticks(*parse_decimal_e9("79418.30"));
    event.qty = *base.qty_to_lots(*parse_decimal_e9("0.5"));
    event.leaves_qty = *base.qty_to_lots(*parse_decimal_e9("1"));
    event.fee = *parse_decimal_e9("0.017");
    event.ts_event = k_ts;
    return event;
}

TEST(AdminStream, AnEventCarriesItsSequenceAsDigitsAndIsNeverCoalesced) {
    const auto base = linear_base();
    const auto frame = order_event_frame("a1", 1042, filled_event(base), base);
    EXPECT_TRUE(frame.starts_with(R"({"type":"event","id":"a1","seq":"1042","order_event":{)")) << frame;
    EXPECT_NE(frame.find(R"("client_order_id":")" + digits(ClientOrderId::make(7, 42).raw) + '"'), std::string::npos)
        << frame;
    EXPECT_NE(frame.find(R"("event_type":"filled")"), std::string::npos) << frame;
    EXPECT_NE(frame.find(R"("price":"79418.3")"), std::string::npos) << frame;
    EXPECT_NE(frame.find(R"("leaves":"1")"), std::string::npos) << frame;
}

TEST(AdminStream, AFillIsTheFilledEventSeenThroughTheFillShape) {
    const auto base = linear_base();
    const auto frame = fill_frame("a1", 7, filled_event(base), base);
    EXPECT_TRUE(frame.starts_with(R"({"type":"event","id":"a1","seq":"7","fill":{)")) << frame;
    EXPECT_NE(frame.find(R"("venue":"deribit","exec_id":"E-1")"), std::string::npos) << frame;
    EXPECT_NE(frame.find(R"("side":"sell")"), std::string::npos) << frame;
    EXPECT_NE(frame.find(R"("liquidity":"maker")"), std::string::npos) << frame;
    EXPECT_NE(frame.find(R"("qty":"0.5")"), std::string::npos) << frame;
    EXPECT_NE(frame.find(R"("fee":"0.017")"), std::string::npos) << frame;
}

TEST(AdminStream, AnOverrunIsSaidOutLoudRatherThanSwallowed) {
    EXPECT_EQ(gap_frame("a1", 17), R"({"type":"gap","id":"a1","missed":"17"})");
}

TEST(AdminStream, AProblemIsRfc9457AndCarriesTheHandleWhenThereIsOne) {
    const Problem problem{
        .title = "no live channel for reconciliation breaks", .status = 501, .detail = "read /v1/recon-breaks"};
    EXPECT_EQ(problem_frame("a1", problem),
              R"({"type":"problem","id":"a1","problem":{"title":)"
              R"("no live channel for reconciliation breaks","status":501,)"
              R"("detail":"read /v1/recon-breaks"}})");
}

} // namespace
} // namespace raikiri::admin
