#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/catalogue.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/market_data.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::binance {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_BINANCE_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// parsers take the unwrapped payload
std::string data_of(std::string_view name) {
    const auto frame = fixture(name);
    const auto inner = unwrap(frame);
    EXPECT_TRUE(inner.has_value());
    return std::string{*inner};
}

TEST(BinanceWire, UnwrapReturnsTheInnerPayload) {
    const auto frame = fixture("ws_book_ticker.json");
    const auto inner = unwrap(frame);
    ASSERT_TRUE(inner.has_value());
    EXPECT_NE(inner->find("\"bookTicker\""), std::string_view::npos);
    EXPECT_EQ(inner->find("\"stream\""), std::string_view::npos);
}

TEST(BinanceWire, UnwrapPassesAnUnwrappedFrameThrough) {
    // a single-stream socket sends the payload bare
    const std::string bare = R"({"e":"bookTicker","s":"BTCUSDT"})";
    const auto inner = unwrap(bare);
    ASSERT_TRUE(inner.has_value());
    EXPECT_EQ(*inner, bare);
}

TEST(BinanceWire, EventDispatchResolvesKindAndSymbol) {
    for (const auto& [file, kind] : {std::pair{"ws_book_ticker.json", EventKind::BookTicker},
                                     std::pair{"ws_agg_trade.json", EventKind::Trade},
                                     std::pair{"ws_mark_price.json", EventKind::MarkPrice},
                                     std::pair{"ws_depth_update.json", EventKind::DepthUpdate}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        EXPECT_EQ(event->symbol, "BTCUSDT") << file;
    }
}

TEST(BinanceWire, BookTickerParsesBothSides) {
    const auto data = data_of("ws_book_ticker.json");
    const auto book = parse_book_ticker(data);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->symbol, "BTCUSDT");
    EXPECT_FALSE(book->bid_price.empty());
    EXPECT_FALSE(book->ask_price.empty());
    EXPECT_FALSE(book->bid_qty.empty());
    EXPECT_FALSE(book->ask_qty.empty());
    EXPECT_GT(book->update_id, 0U);
    // T is when the book changed, E when binance sent it
    EXPECT_GT(book->transaction_ms, 0U);
    EXPECT_GE(book->event_ms, book->transaction_ms);

    // views point into the caller's buffer
    EXPECT_GE(book->symbol.data(), data.data());
    EXPECT_LE(book->symbol.data() + book->symbol.size(), data.data() + data.size());
}

TEST(BinanceWire, AggTradeCarriesAggressorSide) {
    const auto data = data_of("ws_agg_trade.json");
    const auto trade = parse_trade(data);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->symbol, "BTCUSDT");
    EXPECT_FALSE(trade->price.empty());
    EXPECT_FALSE(trade->qty.empty());
    EXPECT_GT(trade->trade_id, 0U);
    EXPECT_GT(trade->trade_ms, 0U);
    // m == true means the buyer was maker
    EXPECT_TRUE(trade->buyer_is_maker);
}

TEST(BinanceWire, MarkPriceCarriesFundingTogetherWithMark) {
    const auto data = data_of("ws_mark_price.json");
    const auto mark = parse_mark_price(data);
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->symbol, "BTCUSDT");
    EXPECT_FALSE(mark->mark_price.empty());
    EXPECT_FALSE(mark->index_price.empty());
    EXPECT_FALSE(mark->funding_rate.empty());
    EXPECT_GT(mark->next_funding_ms, 1'700'000'000'000U);
}

TEST(BinanceWire, DepthUpdateSurfacesTheChainingIds) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto first_data = data_of("ws_depth_update.json");
    const auto first = parse_depth_update(first_data, *storage);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->symbol, "BTCUSDT");
    EXPECT_FALSE(first->bids.empty());
    EXPECT_FALSE(first->asks.empty());
    EXPECT_LE(first->first_id, first->last_id);
    const auto first_last = first->last_id;

    auto next_storage = std::make_unique<LevelBuffer>();
    const auto next_data = data_of("ws_depth_update_next.json");
    const auto next = parse_depth_update(next_data, *next_storage);
    ASSERT_TRUE(next.has_value());
    // on futures pu equals the previous u; these fixtures are consecutive live frames
    EXPECT_EQ(next->prev_id, first_last);
}

TEST(BinanceWire, DepthSnapshotCarriesTheIdTheSeedIsAnchoredOn) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto body = fixture("rest_depth_snapshot.json");
    const auto snapshot = parse_depth_snapshot(body, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->last_update_id, 11'256'392'073'532ULL);
    ASSERT_EQ(snapshot->bids.size(), 5U);
    ASSERT_EQ(snapshot->asks.size(), 5U);
    EXPECT_EQ(snapshot->bids[0].price, "64453.00");
    EXPECT_EQ(snapshot->bids[0].qty, "7.616");
    EXPECT_EQ(snapshot->asks[0].price, "64453.10");
    // a body with no id is refused
    const std::string idless = R"({"bids":[["1.0","1.0"]],"asks":[["2.0","1.0"]]})";
    EXPECT_FALSE(parse_depth_snapshot(idless, *storage).has_value());
}

TEST(BinanceWire, DepthLevelsKeepPriceAndQtyInOrder) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto data = data_of("ws_depth_update.json");
    const auto depth = parse_depth_update(data, *storage);
    ASSERT_TRUE(depth.has_value());
    for (const auto& level : depth->bids) {
        ASSERT_FALSE(level.price.empty());
        ASSERT_FALSE(level.qty.empty());
        EXPECT_NE(level.price.find('.'), std::string_view::npos);
    }
}

TEST(BinanceWire, RestErrorIsDetectedByNegativeCode) {
    for (const auto& [file, code] :
         {std::pair{"rest_error_bad_symbol.json", -1121}, std::pair{"rest_error_bad_apikey.json", -2014}}) {
        const auto body = fixture(file);
        const auto err = parse_rest_error(body);
        ASSERT_TRUE(err.has_value()) << file;
        EXPECT_EQ(err->code, code) << file;
        EXPECT_FALSE(err->msg.empty()) << file;
    }
}

TEST(BinanceWire, SuccessBodyIsNotReadAsAnError) {
    // zero code means a success payload, including bodies with no code and arrays
    for (const char* file : {"rest_premium_index.json", "rest_server_time.json", "rest_depth_snapshot.json"}) {
        const auto body = fixture(file);
        const auto err = parse_rest_error(body);
        ASSERT_TRUE(err.has_value()) << file;
        EXPECT_EQ(err->code, 0) << file;
    }
}

TEST(BinanceWire, ExchangeInfoReadsFiltersRegardlessOfOrder) {
    auto storage = std::make_unique<InstrumentBuffer>();
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_GE(rows->size(), 4U);

    const auto find = [&](std::string_view symbol) -> const WireInstrument* {
        for (const auto& row : *rows) {
            if (row.symbol == symbol) {
                return &row;
            }
        }
        return nullptr;
    };

    const auto* btc = find("BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->contract_type, "PERPETUAL");
    EXPECT_EQ(btc->status, "TRADING");
    EXPECT_EQ(btc->tick_size, "0.10");
    EXPECT_EQ(btc->step_size, "0.001");
    EXPECT_FALSE(btc->min_notional.empty());

    // the wire layer reports every row; the mapper filters
    const auto* settling = find("OMGUSDT");
    ASSERT_NE(settling, nullptr);
    EXPECT_EQ(settling->status, "SETTLING");
    const auto* tradfi = find("XAUUSDT");
    ASSERT_NE(tradfi, nullptr);
    EXPECT_NE(tradfi->contract_type, "PERPETUAL");
}

TEST(BinanceWire, BothTradeSpellingsReadAsOnePrint) {
    // either trade shape must read; only the id key differs
    const auto aggregate = data_of("ws_agg_trade.json");
    const auto from_aggregate = parse_trade(aggregate);
    ASSERT_TRUE(from_aggregate.has_value());
    EXPECT_EQ(from_aggregate->symbol, "BTCUSDT");
    EXPECT_NE(from_aggregate->trade_id, 0U);

    const auto single = data_of("ws_trade.json");
    const auto from_single = parse_trade(single);
    ASSERT_TRUE(from_single.has_value());
    EXPECT_EQ(from_single->symbol, "BTCUSDT");
    EXPECT_NE(from_single->trade_id, 0U) << "the per-trade stream names it t, not a";
    EXPECT_NE(from_single->trade_ms, 0U);
}

TEST(BinanceWire, PremiumIndexCarriesTheFundingTheStreamNeverSends) {
    // one premiumIndex call answers every symbol
    const auto body = fixture("rest_premium_index.json");
    PremiumBuffer storage;
    const auto rows = parse_premium_index(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_FALSE(rows->empty());

    const auto* btc = std::ranges::find_if(*rows, [](const WireMarkPrice& row) {
                          return row.symbol == "BTCUSDT";
                      }).base();
    ASSERT_NE(btc, rows->data() + rows->size());
    EXPECT_FALSE(btc->mark_price.empty());
    EXPECT_FALSE(btc->funding_rate.empty());
    EXPECT_GT(btc->next_funding_ms, 1'700'000'000'000U);
}

TEST(BinanceWire, ServerTimeParses) {
    const auto body = fixture("rest_server_time.json");
    const auto time = parse_server_time(body);
    ASSERT_TRUE(time.has_value());
    EXPECT_GT(*time, 1'700'000'000'000U);
}

TEST(BinanceWire, AccountCarriesEveryCollateralAssetNotOnlyTheHeldOnes) {
    auto storage = std::make_unique<BalanceBuffer>();
    const auto body = fixture("rest_account.json");
    const auto account = parse_account(body, *storage);
    ASSERT_TRUE(account.has_value());
    EXPECT_FALSE(account->multi_assets_margin);
    // twenty rows; map_balance decides which to publish
    ASSERT_EQ(account->assets.size(), 20U);
    EXPECT_EQ(account->assets.front().asset, "USDT");
    EXPECT_EQ(account->assets.front().wallet_balance, "12425.26059217");
    EXPECT_EQ(account->assets.front().available_balance, "11982.40239217");
    EXPECT_EQ(account->assets.front().unrealized_profit, "-18.42910000");
    EXPECT_EQ(account->assets.front().update_ms, 1'787'841'486'006ULL);
    EXPECT_EQ(account->assets.back().asset, "SUI");
    EXPECT_EQ(account->assets.back().wallet_balance, "0.00000000");
    EXPECT_EQ(account->assets.back().update_ms, 0U);

    // an error body is not an empty account
    const std::string_view rejected = R"({"code":-2015,"msg":"Invalid API-key."})";
    EXPECT_FALSE(parse_account(rejected, *storage).has_value());
}

TEST(BinanceWire, GarbageIsRefusedRatherThanPartiallyRead) {
    auto storage = std::make_unique<LevelBuffer>();
    for (const char* junk : {"", "{", "[]", "null", "{\"e\":", "\xff\xfe", "{\"e\":\"bookTicker\"}"}) {
        const std::string_view frame{junk};
        (void)parse_event(frame);
        EXPECT_FALSE(parse_book_ticker(frame).has_value()) << junk;
        EXPECT_FALSE(parse_trade(frame).has_value()) << junk;
        EXPECT_FALSE(parse_depth_update(frame, *storage).has_value()) << junk;
        EXPECT_FALSE(parse_server_time(frame).has_value()) << junk;
    }
}

TEST(BinanceWire, TheTradeAFeeWasChargedOnIsReadAsTheStringBinanceSends) {
    // tradeId is a string on this endpoint while tranId is a number
    const auto body = fixture("rest_income.json");
    IncomeBuffer storage;
    const auto rows = parse_income(body, storage);
    ASSERT_TRUE(rows.has_value()) << to_string(rows.error());
    ASSERT_EQ(rows->size(), 2U);

    EXPECT_EQ((*rows)[0].income_type, "COMMISSION");
    EXPECT_EQ((*rows)[0].trade_id, 220'574'436ULL) << "the string, parsed";
    EXPECT_EQ((*rows)[0].tran_id, 95'210'220'574'436ULL) << "the number beside it, untouched";

    // a funding row has an empty tradeId, read as zero
    EXPECT_EQ((*rows)[1].income_type, "FUNDING_FEE");
    EXPECT_EQ((*rows)[1].trade_id, 0ULL);
    EXPECT_NE((*rows)[1].tran_id, 0ULL);
}

TEST(BinanceWire, TheFundingScheduleIsReadFromTheOnlyPlaceBinanceStatesIt) {
    // one row of each interval binance publishes
    const auto body = fixture("rest_funding_info.json");
    FundingInfoBuffer storage;
    const auto rows = parse_funding_info(body, storage);
    ASSERT_TRUE(rows.has_value()) << to_string(rows.error());
    ASSERT_EQ(rows->size(), 3U);

    EXPECT_EQ((*rows)[0].symbol, "GTCUSDT");
    EXPECT_EQ((*rows)[0].interval_hours, 8U);
    EXPECT_EQ((*rows)[1].symbol, "LPTUSDT");
    EXPECT_EQ((*rows)[1].interval_hours, 4U);
    EXPECT_EQ((*rows)[2].symbol, "ARKUSDT");
    EXPECT_EQ((*rows)[2].interval_hours, 1U);

    EXPECT_EQ((*rows)[0].cap, "0.02000000");
    EXPECT_EQ((*rows)[0].floor, "-0.02000000");
}

TEST(BinanceWire, SubscribeRefusalIsNotAnAcknowledgement) {
    // ack and refusal differ by one field
    const std::string ok = R"({"result":null,"id":1})";
    auto event = parse_event(ok);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::SubscribeAck);

    const std::string bad = R"({"error":{"code":2,"msg":"Invalid request: invalid stream"},"id":7})";
    event = parse_event(bad);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Error);

    const auto err = parse_stream_error(bad);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, 2);
    EXPECT_EQ(err->id, 7U);
    EXPECT_EQ(err->msg, "Invalid request: invalid stream");
}

TEST(BinanceWs, EachStreamGoesToThePathThatDeliversIt) {
    for (const auto kind : {exec::SubKind::Bbo, exec::SubKind::Depth, exec::SubKind::Trades}) {
        EXPECT_EQ(stream_path(kind), "/stream");
    }
    for (const auto kind : {exec::SubKind::MarkPrice, exec::SubKind::Funding}) {
        EXPECT_EQ(stream_path(kind), "/market/stream");
    }
    EXPECT_EQ(stream_host(false), "fstream.binance.com");

    std::array<char, 64> buf{};
    EXPECT_EQ(stream_name(exec::SubKind::MarkPrice, "BTCUSDT", buf), "btcusdt@markPrice@1s");
    EXPECT_EQ(stream_name(exec::SubKind::Trades, "BTCUSDT", buf), "btcusdt@trade");
}

} // namespace
} // namespace raikiri::venues::binance
