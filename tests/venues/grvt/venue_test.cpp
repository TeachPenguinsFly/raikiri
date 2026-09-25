#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/venues/grvt/book.hpp>
#include <raikiri/venues/grvt/endpoints.hpp>
#include <raikiri/venues/grvt/grvt_venue.hpp>
#include <raikiri/venues/grvt/map.hpp>
#include <raikiri/venues/grvt/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace raikiri::venues::grvt {
namespace {

// held: the wire types are views over these bytes
const std::string& fixture(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> held;
    if (const auto found = held.find(name); found != held.end()) {
        return found->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto text = buf.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return held.emplace(std::string{name}, std::move(text)).first->second;
}

constexpr UnixNanos k_ingest{1'790'281'600'000'000'000ULL};

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_order_error(ClientOrderId, const exec::VenueError& error) noexcept override { errors.push_back(error); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
    std::vector<exec::VenueError> errors;
};

exec::InstrumentTable load_table() {
    static const auto storage = std::make_unique<InstrumentBuffer>();
    const auto rows = parse_instruments(fixture("rest_all_instruments.json"), *storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value()) << row.instrument;
        }
    }
    return table;
}

const Instrument& listed(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    return *instrument;
}

WireFeed feed_of(std::string_view name, MessageBuffer& storage) {
    const auto message = parse_message(fixture(name), storage);
    EXPECT_TRUE(message.has_value()) << name;
    EXPECT_EQ(message->kind, MessageKind::Feed) << name;
    return message->feed;
}

TEST(GrvtCatalogue, TheIntervalIsTheOneEachContractStates) {
    const auto table = load_table();
    EXPECT_EQ(table.size(), 3U);
    EXPECT_EQ(table.find("AI16Z_USDT_Perp"), nullptr);

    const auto& btc = std::get<PerpInstrument>(listed(table, "BTC_USDT_Perp"));
    EXPECT_EQ(btc.funding_interval_s, 28'800U);
    EXPECT_EQ(btc.base.tick_size, *parse_decimal_e9("0.1"));
    EXPECT_EQ(btc.base.lot_size, *parse_decimal_e9("0.001"));
    EXPECT_EQ(btc.base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(btc.base.kind, ContractKind::Linear);

    const auto& aster = std::get<PerpInstrument>(listed(table, "ASTER_USDT_Perp"));
    EXPECT_EQ(aster.funding_interval_s, 14'400U);

    const auto& ada = std::get<PerpInstrument>(listed(table, "ADA_USDT_Perp"));
    EXPECT_EQ(ada.funding_cap, *parse_decimal_e9("0.00375"));
    EXPECT_EQ(ada.funding_floor, *parse_decimal_e9("-0.00375"));
}

TEST(GrvtCatalogue, ARowStatingNoIntervalStaysUnstated) {
    WireInstrument row{};
    row.instrument = "NEW_USDT_Perp";
    row.base = "NEW";
    row.quote = "USDT";
    row.kind = "PERPETUAL";
    row.status = "ACTIVE";
    row.tick_size = "0.01";
    row.min_size = "1.0";
    const auto instrument = map_instrument(row, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    const auto& perp = std::get<PerpInstrument>(*instrument);
    EXPECT_EQ(perp.funding_interval_s, 0U);
    EXPECT_TRUE(perp.funding_cap.is_undef());
}

TEST(GrvtCatalogue, NoMarketButPerpIsMapped) {
    InstrumentBuffer storage;
    const auto rows = parse_instruments(fixture("rest_all_instruments.json"), storage);
    ASSERT_TRUE(rows.has_value());
    for (const auto market : {MarketType::Spot, MarketType::Future, MarketType::Option}) {
        EXPECT_FALSE(map_instrument(rows->front(), market).has_value());
    }
}

TEST(GrvtCatalogue, ARestRefusalIsReadWithItsCode) {
    const auto err = parse_rest_error(fixture("rest_error.json"));
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, 1003);
    EXPECT_FALSE(parse_rest_error(fixture("rest_all_instruments.json")).has_value());
}

TEST(GrvtBook, AckSnapshotAndTwoDeltasChainAndApply) {
    DepthBook book{InstrumentId::make(VenueId::Grvt, MarketType::Perp, SymbolId{1}),
                   *parse_decimal_e9("0.01"),
                   *parse_decimal_e9("0.001")};

    MessageBuffer storage;
    const auto ack = parse_message(fixture("ws_book_d_ack.json"), storage);
    ASSERT_TRUE(ack.has_value());
    ASSERT_EQ(ack->kind, MessageKind::Ack);
    ASSERT_EQ(ack->ack.subs.size(), 1U);
    EXPECT_EQ(ack->ack.stream, ep::k_book_stream);
    EXPECT_EQ(selector_instrument(ack->ack.subs[0]), "ETH_USDT_Perp");
    book.sequence.acknowledged(ack->ack.latest[0]);

    const auto snap = feed_of("ws_book_d_snapshot.json", storage);
    ASSERT_EQ(snap.kind, FeedKind::Book);
    EXPECT_EQ(snap.sequence, 0U);
    ASSERT_EQ(book.sequence.offer(snap.sequence, snap.prev_sequence), Step::Snapshot);
    ASSERT_TRUE(book.replace(snap.book));
    const auto seeded = book.ladders().touch();
    ASSERT_TRUE(seeded.has_value());
    EXPECT_EQ(seeded->first.price, *to_ticks("2683.39", *parse_decimal_e9("0.01")));

    const auto first = feed_of("ws_book_d_delta.json", storage);
    ASSERT_EQ(book.sequence.offer(first.sequence, first.prev_sequence), Step::Apply);
    ASSERT_TRUE(book.apply(first.book));

    const auto second = feed_of("ws_book_d_delta_next.json", storage);
    ASSERT_EQ(book.sequence.offer(second.sequence, second.prev_sequence), Step::Apply);
    ASSERT_TRUE(book.apply(second.book));

    const auto top = book.ladders().touch();
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->first.qty, *to_lots("57.259", *parse_decimal_e9("0.001")));
    for (const auto& ask : book.ladders().asks().levels()) {
        EXPECT_NE(ask.price, *to_ticks("2683.86", *parse_decimal_e9("0.01")));
    }
    const auto published = book.snapshot(book.sequence.last(), k_ingest, k_ingest);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->venue_seq, second.sequence);
    EXPECT_EQ(published->bid_count, k_book_levels);
}

TEST(GrvtBook, ADeltaThatDoesNotChainDropsTheBookAndAsksOnce) {
    Sequence sequence;
    sequence.acknowledged(10971);
    ASSERT_EQ(sequence.offer(0, 0), Step::Snapshot);
    ASSERT_EQ(sequence.offer(10972, 10971), Step::Apply);
    EXPECT_EQ(sequence.offer(10972, 10971), Step::Stale);
    EXPECT_EQ(sequence.offer(10975, 10974), Step::Gap);
    EXPECT_FALSE(sequence.live());
    EXPECT_EQ(sequence.gaps(), 1U);
    EXPECT_TRUE(sequence.take_resubscribe());
    EXPECT_FALSE(sequence.take_resubscribe());
    EXPECT_EQ(sequence.offer(10976, 10975), Step::Cold);

    sequence.acknowledged(10980);
    ASSERT_EQ(sequence.offer(0, 0), Step::Snapshot);
    EXPECT_TRUE(sequence.live());
    EXPECT_EQ(sequence.offer(10981, 10980), Step::Apply);
    EXPECT_TRUE(sequence.take_resubscribe());
}

TEST(GrvtBook, ASnapshotWithNoAckAheadOfItIsNotTrusted) {
    Sequence sequence;
    EXPECT_EQ(sequence.offer(0, 0), Step::Gap);
    EXPECT_FALSE(sequence.live());
}

TEST(GrvtBook, ALevelOffTheGridDropsTheWholeBook) {
    const auto tick = *parse_decimal_e9("0.1");
    DepthBook book{InstrumentId::make(VenueId::Grvt, MarketType::Perp, SymbolId{1}), tick, *parse_decimal_e9("0.001")};
    book.sequence.acknowledged(1);
    ASSERT_EQ(book.sequence.offer(0, 0), Step::Snapshot);
    MessageBuffer storage;
    // ETH's own prices step by 0.01, so on a 0.1 grid most of them are off it.
    const auto snap = feed_of("ws_book_d_snapshot.json", storage);
    EXPECT_FALSE(book.replace(snap.book));
    EXPECT_FALSE(book.sequence.live());
    EXPECT_FALSE(book.snapshot(0, k_ingest, k_ingest).has_value());
}

TEST(GrvtQuote, ADeltaChangesOnlyTheFieldsItCarries) {
    const auto tick = *parse_decimal_e9("0.01");
    const auto lot = *parse_decimal_e9("0.001");
    Quote quote{InstrumentId::make(VenueId::Grvt, MarketType::Perp, SymbolId{1}), tick, lot};
    MessageBuffer storage;
    const auto snap = feed_of("ws_mini_d_snapshot.json", storage);
    ASSERT_EQ(snap.kind, FeedKind::Quote);
    const auto delta = feed_of("ws_mini_d_delta.json", storage);
    // the first delta's prev stands in for the uncaptured ack
    quote.sequence.acknowledged(delta.prev_sequence);

    ASSERT_EQ(quote.sequence.offer(snap.sequence, snap.prev_sequence), Step::Snapshot);
    ASSERT_TRUE(quote.replace(snap.quote));
    const auto before = quote.bbo(k_ingest, k_ingest);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->bid, *to_ticks("2683.39", tick));
    EXPECT_EQ(before->ask, *to_ticks("2683.4", tick));
    EXPECT_EQ(before->bid_qty, *to_lots("56.141", lot));

    EXPECT_FALSE(delta.quote.bid_price.has_value());
    ASSERT_EQ(quote.sequence.offer(delta.sequence, delta.prev_sequence), Step::Apply);
    ASSERT_TRUE(quote.apply(delta.quote));
    const auto after = quote.bbo(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->bid_qty, *to_lots("57.259", lot));
    EXPECT_EQ(after->bid, before->bid);
    EXPECT_EQ(after->ask, before->ask);
    EXPECT_EQ(after->ask_qty, before->ask_qty);
}

TEST(GrvtQuote, AnEmptyFieldIsTheVenueNullingIt) {
    const auto tick = *parse_decimal_e9("0.01");
    const auto lot = *parse_decimal_e9("0.001");
    Quote quote{InstrumentId::make(VenueId::Grvt, MarketType::Perp, SymbolId{1}), tick, lot};
    MessageBuffer storage;
    const auto snap = feed_of("ws_mini_d_snapshot.json", storage);
    quote.sequence.acknowledged(1);
    ASSERT_EQ(quote.sequence.offer(0, 0), Step::Snapshot);
    ASSERT_TRUE(quote.replace(snap.quote));
    WireQuote nulled{};
    nulled.ask_price = std::string_view{};
    ASSERT_EQ(quote.sequence.offer(2, 1), Step::Apply);
    ASSERT_TRUE(quote.apply(nulled));
    EXPECT_FALSE(quote.bbo(k_ingest, k_ingest).has_value());
}

TEST(GrvtFeed, ATakerBuyIsABuyAggressor) {
    const auto tick = *parse_decimal_e9("0.01");
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Grvt, MarketType::Perp, SymbolId{1});
    base.tick_size = tick;
    base.lot_size = *parse_decimal_e9("0.001");
    MessageBuffer storage;
    const auto print = feed_of("ws_trade.json", storage);
    ASSERT_EQ(print.kind, FeedKind::Trade);
    EXPECT_NE(print.sequence, 0U);
    const auto trade = map_trade(print.print, base, ns_or(print.event_ns, k_ingest), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price, *to_ticks("2683.4", tick));
    EXPECT_EQ(trade->ts_event.ns, 1'790'281'605'432'856'303ULL);

    EXPECT_EQ(feed_of("ws_trade_snapshot.json", storage).sequence, 0U);
}

TEST(GrvtFeed, TheTickerCarriesFundingAsAPercentOfOneInterval) {
    const auto table = load_table();
    MessageBuffer storage;
    const auto ticker = feed_of("ws_ticker_s.json", storage);
    ASSERT_EQ(ticker.kind, FeedKind::Ticker);
    // ETH's frame against BTC's row: only the grid and the interval are read from it.
    const auto& btc = listed(table, "BTC_USDT_Perp");
    const auto funding = map_funding(ticker.ticker, btc, k_ingest, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // "0.01" is 0.01 %, one basis point.
    EXPECT_EQ(funding->rate, *parse_decimal_e9("0.0001"));
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_EQ(funding->next_funding_time.ns, 1'790'294'400'000'000'000ULL);
    // 2683.383994134 on BTC's 0.1 grid.
    EXPECT_EQ(funding->mark_price.raw, 26'833);

    const auto stats = map_ticker_stats(ticker.ticker, base_of(btc), k_ingest, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, *parse_decimal_e9("28842.081"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("77162625.81605"));
}

TEST(GrvtFeed, AFrameStatingNoRateIsNotFundingAtZero) {
    const auto table = load_table();
    WireTicker ticker{};
    ticker.mark_price = "1.0";
    EXPECT_FALSE(map_funding(ticker, listed(table, "BTC_USDT_Perp"), k_ingest, k_ingest).has_value());
    EXPECT_TRUE(percent_to_ratio("").is_undef());
    EXPECT_EQ(percent_to_ratio("-0.0003"), *parse_decimal_e9("-0.000003"));
}

TEST(GrvtFeed, ARefusedSubscribeIsReadAsAnError) {
    MessageBuffer storage;
    const auto message = parse_message(fixture("ws_error.json"), storage);
    ASSERT_TRUE(message.has_value());
    ASSERT_EQ(message->kind, MessageKind::Error);
    EXPECT_EQ(message->error.code, 3000);
}

TEST(GrvtVenue, EveryOrderPathRefusesWithTheReason) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Grvt, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Grvt);
    ASSERT_TRUE(adapter->execution->start().has_value());
    EXPECT_FALSE(adapter->execution->connected());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Grvt, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.qty = QtyLots::from_raw(7);
    adapter->execution->submit(cmd);
    adapter->execution->cancel(cmd);
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);
    ASSERT_EQ(sink.errors.size(), 3U);
    EXPECT_EQ(sink.errors[0].cls, exec::ErrorClass::Terminal);
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{}).has_value());
}

TEST(GrvtVenue, SpotIsRefusedBeforeAnythingConnects) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto spot = exec::make_venue({.venue = VenueId::Grvt, .market = MarketType::Spot}, sink, clock);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error(), exec::MakeError::UnsupportedMarket);
    const auto& caps = exec::caps_of(VenueId::Grvt);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.ws_order_entry || caps.amend_order || caps.position_pull.any());
}

} // namespace
} // namespace raikiri::venues::grvt
