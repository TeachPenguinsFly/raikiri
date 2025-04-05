#include <raikiri/venues/binance/wire.hpp>

#include <gtest/gtest.h>

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
                                     std::pair{"ws_agg_trade.json", EventKind::AggTrade},
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
    const auto trade = parse_agg_trade(data);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->symbol, "BTCUSDT");
    EXPECT_FALSE(trade->price.empty());
    EXPECT_FALSE(trade->qty.empty());
    EXPECT_GT(trade->agg_id, 0U);
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

TEST(BinanceWire, PremiumIndexParsesFundingFields) {
    const auto body = fixture("rest_premium_index.json");
    const auto premium = parse_premium_index(body);
    ASSERT_TRUE(premium.has_value());
    EXPECT_EQ(premium->symbol, "BTCUSDT");
    EXPECT_FALSE(premium->mark_price.empty());
    EXPECT_FALSE(premium->last_funding_rate.empty());
    EXPECT_GT(premium->next_funding_ms, 1'700'000'000'000U);
}

TEST(BinanceWire, ServerTimeParses) {
    const auto body = fixture("rest_server_time.json");
    const auto time = parse_server_time(body);
    ASSERT_TRUE(time.has_value());
    EXPECT_GT(*time, 1'700'000'000'000U);
}

TEST(BinanceWire, GarbageIsRefusedRatherThanPartiallyRead) {
    auto storage = std::make_unique<LevelBuffer>();
    for (const char* junk :
         {"", "{", "[]", "null", "{\"e\":", "\xff\xfe", "{\"e\":\"bookTicker\"}"}) {
        const std::string_view frame{junk};
        (void)parse_event(frame);
        EXPECT_FALSE(parse_book_ticker(frame).has_value()) << junk;
        EXPECT_FALSE(parse_agg_trade(frame).has_value()) << junk;
        EXPECT_FALSE(parse_depth_update(frame, *storage).has_value()) << junk;
        EXPECT_FALSE(parse_server_time(frame).has_value()) << junk;
    }
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

} // namespace
} // namespace raikiri::venues::binance
