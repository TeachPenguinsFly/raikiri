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

// Every parser takes the payload, not the {"stream":..,"data":..} wrapper.
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
    // A single-stream socket sends the payload bare; the same parser has to
    // read both, because which socket delivered a frame is not the parser's
    // business.
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
    // T is when the book changed, E is when binance sent it; conflating them
    // would put venue queueing delay into our latency numbers.
    EXPECT_GT(book->transaction_ms, 0U);
    EXPECT_GE(book->event_ms, book->transaction_ms);

    // Views point into the caller's buffer, not into parser scratch.
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
    // m == true means the BUYER was the maker, so the aggressor sold. The
    // field reads backwards from how it is usually wanted, which is exactly
    // why it stays named after the wire here and is flipped in the mapper.
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
    // The futures chaining rule: pu of an event equals u of the one before.
    // The spot rule (U == previous u + 1) rejects every valid futures delta,
    // and these two fixtures are consecutive frames off the live socket.
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
    // Without an id the levels are a picture of no particular moment, so a
    // body missing it is refused rather than read as a book at id zero.
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
    for (const auto& [file, code] : {std::pair{"rest_error_bad_symbol.json", -1121},
                                     std::pair{"rest_error_bad_apikey.json", -2014}}) {
        const auto body = fixture(file);
        const auto err = parse_rest_error(body);
        ASSERT_TRUE(err.has_value()) << file;
        EXPECT_EQ(err->code, code) << file;
        EXPECT_FALSE(err->msg.empty()) << file;
    }
}

TEST(BinanceWire, SuccessBodyIsNotReadAsAnError) {
    // Zero code means "this body is a success payload" — the distinction has
    // to survive bodies that carry no code field at all, and bodies that are
    // arrays rather than objects.
    for (const char* file :
         {"rest_premium_index.json", "rest_server_time.json", "rest_depth_snapshot.json"}) {
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

    // The wire layer reports what the venue said; deciding that a SETTLING
    // contract or a non-perpetual one is unusable belongs to the mapper.
    const auto* settling = find("OMGUSDT");
    ASSERT_NE(settling, nullptr);
    EXPECT_EQ(settling->status, "SETTLING");
    const auto* tradfi = find("XAUUSDT");
    ASSERT_NE(tradfi, nullptr);
    EXPECT_NE(tradfi->contract_type, "PERPETUAL");
}

TEST(BinanceWire, BothTradeSpellingsReadAsOnePrint) {
    // The adapter asks for @trade, because @aggTrade is acknowledged on this
    // venue and then never sends a frame. Either shape must still read: the
    // only difference is which key carries the id.
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
    // The @markPrice stream is acknowledged on this venue and then sends
    // nothing, so this pull is where funding comes from. One call answers for
    // every symbol, which is why the parser fills a buffer rather than a row.
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
    // The mode is why this endpoint is read rather than /fapi/v2/balance: the
    // wallet rows are on both, and only this one says how to read their free
    // figure.
    EXPECT_FALSE(account->multi_assets_margin);
    // Twenty rows, two of them money. The parser reports what arrived; which
    // rows are worth publishing is map_balance's decision, not this one's.
    ASSERT_EQ(account->assets.size(), 20U);
    EXPECT_EQ(account->assets.front().asset, "USDT");
    EXPECT_EQ(account->assets.front().wallet_balance, "12425.26059217");
    EXPECT_EQ(account->assets.front().available_balance, "11982.40239217");
    EXPECT_EQ(account->assets.front().unrealized_profit, "-18.42910000");
    EXPECT_EQ(account->assets.front().update_ms, 1'787'841'486'006ULL);
    EXPECT_EQ(account->assets.back().asset, "SUI");
    EXPECT_EQ(account->assets.back().wallet_balance, "0.00000000");
    EXPECT_EQ(account->assets.back().update_ms, 0U);

    // An error body is not an account with nothing in it.
    const std::string_view rejected = R"({"code":-2015,"msg":"Invalid API-key."})";
    EXPECT_FALSE(parse_account(rejected, *storage).has_value());
}

TEST(BinanceWire, GarbageIsRefusedRatherThanPartiallyRead) {
    auto storage = std::make_unique<LevelBuffer>();
    for (const char* junk :
         {"", "{", "[]", "null", "{\"e\":", "\xff\xfe", "{\"e\":\"bookTicker\"}"}) {
        const std::string_view frame{junk};
        (void)parse_event(frame);
        EXPECT_FALSE(parse_book_ticker(frame).has_value()) << junk;
        EXPECT_FALSE(parse_trade(frame).has_value()) << junk;
        EXPECT_FALSE(parse_depth_update(frame, *storage).has_value()) << junk;
        EXPECT_FALSE(parse_server_time(frame).has_value()) << junk;
    }
}

TEST(BinanceWire, TheTradeAFeeWasChargedOnIsReadAsTheStringBinanceSends) {
    // THE ONE ID BINANCE SENDS AS A STRING. `tranId` beside it in the same
    // object is a bare number, and libs/wire's own note says binance spells
    // its ids as numbers — which is true everywhere except here.
    //
    // Read with opt_u64 it was 0 on every row binance has ever sent. The fee
    // ledger then keyed each charge by `tranId`, which no execution quotes, so
    // not one commission ever joined the fill it belonged to: two live fills on
    // 2026-09-16 produced `fees: agreed=0 unmatched=2`, and the same would have
    // been true of every fee before them.
    const auto body = fixture("rest_income.json");
    IncomeBuffer storage;
    const auto rows = parse_income(body, storage);
    ASSERT_TRUE(rows.has_value()) << to_string(rows.error());
    ASSERT_EQ(rows->size(), 2U);

    EXPECT_EQ((*rows)[0].income_type, "COMMISSION");
    EXPECT_EQ((*rows)[0].trade_id, 220'574'436ULL) << "the string, parsed";
    EXPECT_EQ((*rows)[0].tran_id, 95'210'220'574'436ULL) << "the number beside it, untouched";

    // A FUNDING ROW HAS NO TRADE, and binance says so with "" rather than by
    // omitting the field. Zero is the right reading; what must not happen is
    // the row being refused over it.
    EXPECT_EQ((*rows)[1].income_type, "FUNDING_FEE");
    EXPECT_EQ((*rows)[1].trade_id, 0ULL);
    EXPECT_NE((*rows)[1].tran_id, 0ULL);
}

TEST(BinanceWire, TheFundingScheduleIsReadFromTheOnlyPlaceBinanceStatesIt) {
    // exchangeInfo carries every other trading rule for a perp and not this
    // one, so the catalogue answered eight hours for all 571 of them. Binance
    // settles 465 every four hours and five every hour.
    //
    // The fixture is one row of each interval binance publishes.
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

    // THE CAPS WERE IN THIS FIXTURE FROM THE DAY IT WAS CAPTURED, read by
    // nothing. A rate resting on its ceiling is demand the venue stopped
    // pricing, and an analysis that cannot see the ceiling reads it as a rate
    // that merely happened to be high.
    EXPECT_EQ((*rows)[0].cap, "0.02000000");
    EXPECT_EQ((*rows)[0].floor, "-0.02000000");
}

TEST(BinanceWire, SubscribeRefusalIsNotAnAcknowledgement) {
    // The two answers differ by one field. Reading a refusal as an ack turns
    // "the venue rejected this stream" into "this stream is quiet", which is
    // indistinguishable from a market with no activity.
    const std::string ok = R"({"result":null,"id":1})";
    auto event = parse_event(ok);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::SubscribeAck);

    const std::string bad =
        R"({"error":{"code":2,"msg":"Invalid request: invalid stream"},"id":7})";
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
