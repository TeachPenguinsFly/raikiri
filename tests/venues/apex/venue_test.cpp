#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/apex/book.hpp>
#include <raikiri/venues/apex/endpoints.hpp>
#include <raikiri/venues/apex/map.hpp>
#include <raikiri/venues/apex/wire.hpp>
#include <raikiri/venues/apex/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::apex {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'314'830'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    EXPECT_TRUE(value.has_value()) << text;
    return value.value_or(FixedE9{});
}

WireContract contract(std::string_view cross_symbol) {
    static const auto body = fixture("rest_symbols.json");
    static const auto storage = std::make_unique<ContractBuffer>();
    const auto rows = parse_symbols(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.cross_symbol == cross_symbol) {
            return row;
        }
    }
    ADD_FAILURE() << "no contract " << cross_symbol;
    return {};
}

PerpInstrument perp_for(std::string_view cross_symbol) {
    const auto instrument = map_instrument(contract(cross_symbol), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    auto perp = std::get<PerpInstrument>(*instrument);
    perp.base.id = InstrumentId::make(VenueId::Apex, MarketType::Perp, SymbolId{1});
    return perp;
}

TEST(ApexCatalogue, ReadsPerpsAndStocksButNotPredictions) {
    const auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_symbols.json");
    const auto rows = parse_symbols(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 6U);
    EXPECT_EQ((*rows)[0].cross_symbol, "BTCUSDT");
    EXPECT_EQ((*rows)[4].cross_symbol, "IWMUSDT");
    EXPECT_EQ((*rows)[5].cross_symbol, "XAUUSDT");
}

TEST(ApexCatalogue, APerpIsOneCoinSettledInUsdtHourly) {
    const auto perp = perp_for("BTCUSDT");
    EXPECT_EQ(perp.base.exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(perp.base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(perp.base.tick_size, e9("0.1"));
    EXPECT_EQ(perp.base.lot_size, e9("0.001"));
    EXPECT_EQ(perp.base.contract_value, e9("1"));
    EXPECT_EQ(perp.base.kind, ContractKind::Linear);
    EXPECT_TRUE(perp.base.min_notional.is_undef());
    EXPECT_EQ(perp.funding_interval_s, 3600U);
}

TEST(ApexCatalogue, TheFundingCapIsReadPerContract) {
    EXPECT_EQ(perp_for("BTCUSDT").funding_cap, e9("0.000469"));
    EXPECT_EQ(perp_for("BTCUSDT").funding_floor, e9("-0.000469"));
    EXPECT_EQ(perp_for("TAOUSDT").funding_cap, e9("0.02"));
    EXPECT_EQ(perp_for("IOUSDT").funding_cap, e9("0.000305"));
}

TEST(ApexCatalogue, AStockPerpLoadsLikeAnyOther) {
    const auto xau = perp_for("XAUUSDT");
    EXPECT_EQ(xau.base.local_symbol.view(), "XAU/USDT");
    EXPECT_EQ(xau.base.tick_size, e9("0.01"));
    EXPECT_EQ(xau.funding_interval_s, k_funding_interval_s);
}

TEST(ApexCatalogue, DisabledContractsAndOtherMarketsAreRefused) {
    EXPECT_FALSE(map_instrument(contract("SUNDOGUSDT"), MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(contract("IWMUSDT"), MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(contract("BTCUSDT"), MarketType::Spot).has_value());
    EXPECT_TRUE(map_instrument(contract("IOUSDT"), MarketType::Perp).has_value());
}

TEST(ApexEnvelope, EveryFrameIsClassified) {
    const auto kind = [](std::string_view name) {
        const auto frame = fixture(name);
        const auto topic = parse_topic(frame);
        EXPECT_TRUE(topic.has_value()) << name;
        return topic.value_or(TopicInfo{}).kind;
    };
    EXPECT_EQ(kind("ws_orderbook_snapshot.json"), TopicKind::Orderbook);
    EXPECT_EQ(kind("ws_trade_delta.json"), TopicKind::Trade);
    EXPECT_EQ(kind("ws_ticker_delta.json"), TopicKind::Instrument);
    EXPECT_EQ(kind("ws_server_ping.json"), TopicKind::Ping);
    EXPECT_EQ(kind("ws_subscribed.json"), TopicKind::Ack);
    EXPECT_EQ(kind("ws_refused.json"), TopicKind::Ack);
    EXPECT_EQ(classify_topic("orderBook25.H.ETHUSDT").symbol, "ETHUSDT");
}

TEST(ApexEnvelope, ARefusalNamesEveryTopicItDropped) {
    AckBuffer storage;
    const auto frame = fixture("ws_refused.json");
    const auto ack = parse_ack(frame, storage);
    ASSERT_TRUE(ack.has_value());
    EXPECT_FALSE(ack->success);
    EXPECT_EQ(ack->ret_msg, "error:handler not found");
    EXPECT_EQ(ack->op, "subscribe");
    ASSERT_EQ(ack->args.size(), 2U);
    EXPECT_EQ(ack->args[0], "instrumentInfo.H.ETHUSDT");
    EXPECT_EQ(ack->args[1], "orderBook25.H.NOPEUSDT");
}

TEST(ApexEnvelope, AnAlreadyHeldTopicAlsoRefusesItsFrame) {
    AckBuffer storage;
    const auto frame = fixture("ws_refused_already_subscribed.json");
    const auto ack = parse_ack(frame, storage);
    ASSERT_TRUE(ack.has_value());
    EXPECT_FALSE(ack->success);
    EXPECT_NE(ack->ret_msg.find("already subscribed"), std::string_view::npos);
    EXPECT_EQ(ack->args.size(), 2U);
}

TEST(ApexEnvelope, TheServerPingIsEchoed) {
    const auto frame = fixture("ws_server_ping.json");
    const auto value = parse_ping(frame);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "1790314841517");
    Protocol protocol;
    const auto pong = protocol.pong_frame(*value);
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(*pong, R"({"op":"pong","args":["1790314841517"]})");
}

TEST(ApexWs, TopicsAndFrames) {
    std::array<char, 64> buf{};
    EXPECT_EQ(topic_name(exec::SubKind::Bbo, "BTCUSDT", buf), "orderBook25.H.BTCUSDT");
    EXPECT_EQ(topic_name(exec::SubKind::Depth, "BTCUSDT", buf), "orderBook25.H.BTCUSDT");
    EXPECT_EQ(topic_name(exec::SubKind::Trades, "BTCUSDT", buf), "recentlyTrade.H.BTCUSDT");
    EXPECT_EQ(topic_name(exec::SubKind::Funding, "BTCUSDT", buf), "instrumentInfo.H.BTCUSDT");
    EXPECT_TRUE(topic_name(exec::SubKind::Kline, "BTCUSDT", buf).empty());

    Protocol protocol;
    const std::array<std::string_view, 2> two{"orderBook25.H.BTCUSDT", "recentlyTrade.H.BTCUSDT"};
    const auto frame = protocol.subscribe_frame(two);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, R"({"op":"subscribe","args":["orderBook25.H.BTCUSDT","recentlyTrade.H.BTCUSDT"]})");
}

class ApexBook : public ::testing::Test {
protected:
    WireOrderbook load(std::string_view name) {
        frames_.emplace_back(fixture(name));
        storage_.push_back(std::make_unique<OrderbookBuffer>());
        const auto wire = parse_orderbook(frames_.back(), *storage_.back());
        EXPECT_TRUE(wire.has_value()) << name;
        return wire.value_or(WireOrderbook{});
    }

    InstrumentId id_ = InstrumentId::make(VenueId::Apex, MarketType::Perp, SymbolId{2});
    BookSync book_{id_, e9("0.01"), e9("0.01")};
    std::deque<std::string> frames_;
    std::vector<std::unique_ptr<OrderbookBuffer>> storage_;
};

TEST_F(ApexBook, TheSnapshotSeedsAndDeltasChainOnU) {
    const auto snapshot = load("ws_orderbook_snapshot.json");
    EXPECT_TRUE(snapshot.is_snapshot);
    EXPECT_EQ(snapshot.symbol, "ETHUSDT");
    EXPECT_EQ(snapshot.update_id, 3063780U);
    EXPECT_EQ(snapshot.bids.size(), 25U);
    EXPECT_EQ(snapshot.ts_us, 1790314829373876U);
    ASSERT_EQ(book_.apply(snapshot), SyncResult::Resynced);

    const auto bbo = book_.bbo(k_ingest, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid, PriceTicks::from_raw(268285));
    EXPECT_EQ(bbo->ask, PriceTicks::from_raw(268289));

    const auto delta = load("ws_orderbook_delta.json");
    EXPECT_FALSE(delta.is_snapshot);
    ASSERT_EQ(book_.apply(delta), SyncResult::Applied);
    for (const auto& level : book_.bids()) {
        EXPECT_NE(level.price, PriceTicks::from_raw(268040)) << "a size of 0 deletes the level";
    }
    EXPECT_EQ(book_.apply(delta), SyncResult::Ignored);
    EXPECT_EQ(book_.apply(load("ws_orderbook_delta_next.json")), SyncResult::Applied);
    EXPECT_EQ(book_.last_update_id(), 3063782U);
}

TEST_F(ApexBook, ASkippedUpdateDropsTheBook) {
    ASSERT_EQ(book_.apply(load("ws_orderbook_snapshot.json")), SyncResult::Resynced);
    EXPECT_EQ(book_.apply(load("ws_orderbook_delta_next.json")), SyncResult::GapDetected);
    EXPECT_FALSE(book_.synced());
    EXPECT_FALSE(book_.bbo(k_ingest, k_ingest).has_value());
    EXPECT_EQ(book_.gaps(), 1U);
    EXPECT_EQ(book_.apply(load("ws_orderbook_delta.json")), SyncResult::Ignored);
}

TEST_F(ApexBook, ADeltaBeforeAnySnapshotIsIgnored) {
    EXPECT_EQ(book_.apply(load("ws_orderbook_delta.json")), SyncResult::Ignored);
}

TEST(ApexTrades, TheSubscribeBacklogIsMarkedAndTheTakerSideRead) {
    const auto storage = std::make_unique<TradeBuffer>();
    const auto backlog_frame = fixture("ws_trade_snapshot.json");
    const auto backlog = parse_trades(backlog_frame, *storage);
    ASSERT_TRUE(backlog.has_value());
    EXPECT_TRUE(backlog->is_snapshot);
    EXPECT_EQ(backlog->trades.size(), 50U);

    const auto frame = fixture("ws_trade_delta.json");
    const auto live = parse_trades(frame, *storage);
    ASSERT_TRUE(live.has_value());
    EXPECT_FALSE(live->is_snapshot);
    ASSERT_EQ(live->trades.size(), 1U);

    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Apex, MarketType::Perp, SymbolId{2});
    base.tick_size = e9("0.01");
    base.lot_size = e9("0.01");
    const auto trade = map_trade(live->trades[0], base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price, PriceTicks::from_raw(268221));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(124));
    EXPECT_EQ(trade->ts_event, UnixNanos{1'790'314'839'756'000'000ULL});
}

TEST(ApexFunding, TheSnapshotStatesRateMarkAndNextHour) {
    const auto perp = perp_for("BTCUSDT");
    const auto frame = fixture("ws_ticker_snapshot.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTCUSDT");
    FundingMemo memo;
    const auto funding = map_funding(*wire, perp, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.00000448"));
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(842749)); // 84274.95 onto a 0.1 grid
    EXPECT_EQ(funding->index_price, PriceTicks::from_raw(843161));
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->next_funding_time, UnixNanos{1'790'316'000ULL * 1'000'000'000ULL});
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->ts_event, UnixNanos{1'790'314'829'372'790'000ULL});

    const auto stats = map_ticker_stats(*wire, perp.base, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("3764.689"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("316688835.9246"));
    EXPECT_EQ(stats->open_interest, e9("1564.866"));
}

TEST(ApexFunding, APartialDeltaKeepsWhatWasSaidBefore) {
    const auto perp = perp_for("BTCUSDT");
    FundingMemo memo;
    const auto snapshot_frame = fixture("ws_ticker_snapshot.json");
    ASSERT_TRUE(map_funding(*parse_ticker(snapshot_frame), perp, memo, k_ingest).has_value());

    const auto status_frame = fixture("ws_ticker_delta.json");
    const auto status = parse_ticker(status_frame);
    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(map_funding(*status, perp, memo, k_ingest).has_value());
    EXPECT_FALSE(map_ticker_stats(*status, perp.base, k_ingest).has_value());

    const auto mark_frame = fixture("ws_ticker_delta_mark.json");
    const auto funding = map_funding(*parse_ticker(mark_frame), perp, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.00000448"));
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(842747));
    EXPECT_EQ(funding->next_funding_time, UnixNanos{1'790'316'000ULL * 1'000'000'000ULL});
}

TEST(ApexFunding, ARateNeverSeenStaysUndef) {
    const auto perp = perp_for("BTCUSDT");
    FundingMemo memo;
    const auto frame = fixture("ws_ticker_delta_mark.json");
    const auto funding = map_funding(*parse_ticker(frame), perp, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_TRUE(funding->rate.is_undef());
    EXPECT_TRUE(funding->next_funding_time.ns == UnixNanos{}.ns);
}

TEST(ApexFunding, ARateFinerThanE9IsTruncatedNotDropped) {
    const auto perp = perp_for("BTCUSDT");
    FundingMemo memo;
    WireTicker wire{};
    wire.symbol = "BTCUSDT";
    wire.funding_rate = "-0.0000056612345678";
    const auto funding = map_funding(wire, perp, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, FixedE9::from_raw(-5661));
}

TEST(ApexTime, OnlyWholeUtcSeconds) {
    EXPECT_EQ(parse_utc("2024-02-29T23:59:59Z"), UnixNanos{1'709'251'199ULL * 1'000'000'000ULL});
    EXPECT_FALSE(parse_utc("2026-02-29T00:00:00Z").has_value());
    EXPECT_FALSE(parse_utc("2026-09-25T06:00:00").has_value());
    EXPECT_FALSE(parse_utc("2026-09-25T24:00:00Z").has_value());
    EXPECT_FALSE(parse_utc("").has_value());
}

TEST(ApexCaps, PerpMarketDataOnly) {
    const auto& caps = exec::caps_of(VenueId::Apex);
    EXPECT_EQ(caps.venue, VenueId::Apex);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.book_checksum);
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::None);
    EXPECT_EQ(caps.max_client_order_id_len, 0U);
    EXPECT_EQ(to_string(VenueId::Apex), "apex");
    EXPECT_EQ(ep::k_ban_status, 403);
}

} // namespace
} // namespace raikiri::venues::apex
