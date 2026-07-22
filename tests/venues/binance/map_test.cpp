#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::binance {
namespace {

// Only the wire target carries RAIKIRI_BINANCE_FIXTURES, so the book tests
// locate the directory from this file the way the gate target does.
std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// BTCUSDT as binance publishes it: 0.10 tick, 0.001 step.
exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.base_asset = "BTC";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.min_notional = "5";
    // BTCUSDT's own schedule, stamped the way the catalogue loader stamps it
    // from fundingInfo. It has to be said here: the funding memo carries the
    // interval down from the interned perp, so a table built without one
    // describes an instrument whose schedule binance never stated.
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

TEST(BinanceMap, BookTickerBecomesBbo) {
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.symbol = "BTCUSDT";
    wire.bid_price = "64488.50";
    wire.bid_qty = "5.560";
    wire.ask_price = "64488.60";
    wire.ask_qty = "4.245";
    wire.transaction_ms = 1'786'375'246'658ULL;
    wire.event_ms = 1'786'375'246'659ULL;

    const auto bbo = map_bbo(wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 644'885); // 64488.50 / 0.10
    EXPECT_EQ(bbo->ask.raw, 644'886);
    EXPECT_EQ(bbo->bid_qty.raw, 5'560); // 5.560 / 0.001
    EXPECT_EQ(bbo->ask_qty.raw, 4'245);
    // ts_event is T, not E — the moment the book changed.
    EXPECT_EQ(bbo->ts_event.ns, 1'786'375'246'658ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(BinanceMap, PriceOffTheTickIsDroppedNotRounded) {
    // A price that does not divide by the tick means binance changed a filter
    // under us. Rounding would put an order one tick from where it was meant.
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.symbol = "BTCUSDT";
    wire.bid_price = "64488.55"; // 0.05 past a 0.10 tick
    wire.bid_qty = "1.000";
    wire.ask_price = "64488.60";
    wire.ask_qty = "1.000";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, UnknownSymbolIsDropped) {
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.symbol = "DOGEUSDT";
    wire.bid_price = "0.10";
    wire.bid_qty = "1.000";
    wire.ask_price = "0.20";
    wire.ask_qty = "1.000";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, MakerFlagIsInvertedIntoAggressorSide) {
    // This is the field most likely to be read backwards. m == true means the
    // buyer was the maker, so the aggressor SOLD.
    const auto table = table_with_btc();
    WirePrint wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "64357.70";
    wire.qty = "0.008";
    wire.trade_ms = 1'786'375'704'122ULL;

    wire.buyer_is_maker = true;
    auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);

    wire.buyer_is_maker = false;
    trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 643'577);
    EXPECT_EQ(trade->qty.raw, 8);
}

TEST(BinanceMap, MissingVenueTimeFallsBackToIngest) {
    const auto table = table_with_btc();
    WirePrint wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "64357.70";
    wire.qty = "0.008";
    wire.trade_ms = 0;
    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // Not 1970: a zero timestamp reaching a latency histogram poisons it.
    EXPECT_EQ(trade->ts_event.ns, k_ingest.ns);
}

TEST(BinanceMap, MarkPriceBecomesFunding) {
    const auto table = table_with_btc();
    WireMarkPrice wire{};
    wire.symbol = "BTCUSDT";
    wire.mark_price = "64456.50000000";
    wire.index_price = "64484.26195652";
    wire.funding_rate = "0.00004806";
    wire.next_funding_ms = 1'786'377'600'000ULL;
    wire.event_ms = 1'786'375'403'002ULL;

    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 48'060); // 0.00004806 * 1e9
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'377'600'000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_GT(funding->mark_price.raw, 0);
}

TEST(BinanceMap, MarkPriceOffTheTickStillMaps) {
    // Mark is an index-weighted average and is NOT rounded to the trading
    // tick, so the exact-step rule cannot apply to it — demanding
    // divisibility here would drop every funding frame binance sends.
    const auto table = table_with_btc();
    WireMarkPrice wire{};
    wire.symbol = "BTCUSDT";
    wire.mark_price = "64456.53700000";
    wire.funding_rate = "0.00004806";
    wire.next_funding_ms = 1'786'377'600'000ULL;
    EXPECT_TRUE(map_funding(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, ContractWithoutFundingIsNotReportedAsZero) {
    const auto table = table_with_btc();
    WireMarkPrice wire{};
    wire.symbol = "BTCUSDT";
    wire.mark_price = "64456.50";
    wire.funding_rate = {}; // absent, not "0"
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, NonPerpetualAndNonTradingRowsAreRefused) {
    WireInstrument wire{};
    wire.symbol = "XAUUSDT";
    wire.contract_type = "TRADIFI_PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value())
        << "asking for perps and getting a tradfi-hours contract is a position bug";

    wire.symbol = "OMGUSDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "SETTLING";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.status = "PENDING_TRADING";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(BinanceMap, InstrumentWithoutUsableStepsIsRefused) {
    // Every later conversion divides by these, so a zero step must never
    // reach the table.
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.tick_size = "0.10";
    wire.step_size = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(BinanceMap, CatalogueRowsCarryTheCanonicalPair) {
    // baseAsset and quoteAsset ride every /fapi/v1/exchangeInfo row, so the
    // venue-neutral name costs nothing to build here.
    auto storage = std::make_unique<InstrumentBuffer>();
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, *storage);
    ASSERT_TRUE(rows.has_value());

    std::size_t mapped = 0;
    for (const auto& row : *rows) {
        const auto instrument = map_instrument(row, MarketType::Perp);
        if (!instrument.has_value()) {
            continue;
        }
        const auto& base = base_of(*instrument);
        EXPECT_EQ(base.exchange_symbol.view(), row.symbol);
        if (row.symbol == "BTCUSDT") {
            EXPECT_EQ(base.local_symbol.view(), "BTC/USDT");
        } else if (row.symbol == "ETHUSDT") {
            EXPECT_EQ(base.local_symbol.view(), "ETH/USDT");
        } else if (row.symbol == "XAUUSDT") {
            EXPECT_EQ(base.local_symbol.view(), "XAU/USDT");
        }
        ++mapped;
    }
    EXPECT_EQ(mapped, 3U) << "OMGUSDT is settling";
}

TEST(BinanceMap, RowWithoutAPairIsDropped) {
    // An instrument nothing can group shows in a cross-venue view as a market
    // of its own, which is worse than not carrying it.
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.base_asset = "BTC";
    wire.quote_asset = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.quote_asset = "USDT";
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base_of(*instrument).exchange_symbol.view(), "BTCUSDT");
}

TEST(BinanceErrors, AuthCodesHaltTheVenue) {
    for (const auto code :
         {k_unauthorized, k_invalid_signature, k_bad_api_key_format, k_rejected_key_or_ip}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::AuthFailure) << code;
    }
}

TEST(BinanceErrors, ClockSkewIsRetryableNotTerminal) {
    // -1021 is the single most common false terminal: the request was fine,
    // our clock was not. Resync and resend succeeds.
    EXPECT_EQ(classify(k_invalid_timestamp), exec::ErrorClass::Retryable);
}

TEST(BinanceErrors, ExpiredListenKeyIsRetryableAndNotAuthFailure) {
    EXPECT_EQ(classify(k_invalid_listen_key), exec::ErrorClass::Retryable);
    EXPECT_TRUE(is_listen_key_failure(k_invalid_listen_key));
    EXPECT_FALSE(is_listen_key_failure(k_invalid_signature));
}

TEST(BinanceErrors, RateLimitCodesBackOff) {
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_too_many_orders), exec::ErrorClass::RateLimited);
}

TEST(BinanceErrors, UnknownCodeIsTerminalByDefault) {
    // Deliberate: an unknown refusal retried at full speed earns an IP ban,
    // while a spuriously terminal order is recovered by reconciliation.
    EXPECT_EQ(classify(-999'999), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_bad_symbol), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_post_only_would_take), exec::ErrorClass::Terminal);
}

TEST(BinanceErrors, BenignCancelRacesAreRecognised) {
    EXPECT_TRUE(is_benign_cancel_failure(k_no_such_order));
    EXPECT_TRUE(is_benign_cancel_failure(k_cancel_rejected));
    EXPECT_FALSE(is_benign_cancel_failure(k_balance_insufficient));
}

// --- private stream -------------------------------------------------------

WireOrderUpdate order_update(std::string_view exec_type, std::string_view status) {
    WireOrderUpdate wire{};
    wire.symbol = "BTCUSDT";
    wire.client_order_id = "01HQ8ZK9M4TESTULID000000AB";
    wire.exec_type = exec_type;
    wire.order_status = status;
    wire.side = "BUY";
    wire.order_type = "LIMIT";
    wire.time_in_force = "GTC";
    wire.order_qty = "0.010";
    wire.order_price = "64488.50";
    wire.last_filled_qty = "0.000";
    wire.cum_filled_qty = "0.000";
    wire.last_filled_price = "0";
    wire.order_id = 4611875134427365377ULL;
    wire.event_ms = 1'786'375'246'659ULL;
    wire.trade_ms = 1'786'375'246'658ULL;
    return wire;
}

TEST(BinanceMap, ExecutionTypeDecidesTheEventNotOrderStatus) {
    // The double-count hazard. A TRADE frame also reports X = FILLED; if the
    // status were turned into an event of its own, every fill would be
    // counted twice and every position would double.
    const auto table = table_with_btc();

    auto wire = order_update("TRADE", "FILLED");
    wire.last_filled_qty = "0.004";
    wire.cum_filled_qty = "0.010";
    wire.last_filled_price = "64488.60";
    wire.commission = "0.01289772";
    wire.is_maker = false;

    const auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    // The FILL's own price and size, not the resting order's.
    EXPECT_EQ(event->price.raw, 644'886);
    EXPECT_EQ(event->qty.raw, 4);
    EXPECT_EQ(event->leaves_qty.raw, 0);
    EXPECT_EQ(event->liquidity, Liquidity::Taker) << "m == false means we took liquidity";
    EXPECT_GT(event->fee.raw, 0);
}

TEST(BinanceMap, LifecycleTransitionsComeFromExecutionTypeToo) {
    const auto table = table_with_btc();
    for (const auto& [exec_type, expected] : {std::pair{"NEW", OrderEventType::Accepted},
                                              std::pair{"CANCELED", OrderEventType::Canceled},
                                              std::pair{"EXPIRED", OrderEventType::Expired},
                                              std::pair{"AMENDMENT", OrderEventType::Updated}}) {
        const auto event = map_order_event(order_update(exec_type, "NEW"), table, k_ingest);
        ASSERT_TRUE(event.has_value()) << exec_type;
        EXPECT_EQ(event->type, expected) << exec_type;
        // A non-fill frame describes the resting order.
        EXPECT_EQ(event->price.raw, 644'885) << exec_type;
        EXPECT_EQ(event->qty.raw, 10) << exec_type;
    }
}

TEST(BinanceMap, LiquidationBookkeepingProducesNoOrderEvent) {
    // CALCULATED is ADL/liquidation accounting, not a transition of an order
    // we submitted. Synthesizing one would apply a transition the FSM has no
    // arm for and refuse it loudly for no reason.
    const auto table = table_with_btc();
    EXPECT_FALSE(map_order_event(order_update("CALCULATED", "NEW"), table, k_ingest).has_value());
}

TEST(BinanceMap, OrdersWeDidNotMintAreDropped) {
    // Binance mints its own client ids for liquidations ("autoclose-..."),
    // longer than the field reserved for our ULIDs. The store has no such
    // order, so the frame is not ours to apply.
    const auto table = table_with_btc();
    auto wire = order_update("TRADE", "FILLED");
    wire.client_order_id = "autoclose-1234567890123456789012345678901234567890";
    wire.last_filled_qty = "0.004";
    wire.last_filled_price = "64488.60";
    EXPECT_FALSE(map_order_event(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, PartialFillReportsWhatIsLeft) {
    const auto table = table_with_btc();
    auto wire = order_update("TRADE", "PARTIALLY_FILLED");
    wire.last_filled_qty = "0.004";
    wire.cum_filled_qty = "0.004";
    wire.last_filled_price = "64488.60";
    const auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->qty.raw, 4);
    EXPECT_EQ(event->leaves_qty.raw, 6) << "0.010 ordered minus 0.004 done";
}

TEST(BinanceMap, PositionAmountKeepsItsSign) {
    // A short is a negative position. Losing the sign here inverts the book.
    const auto table = table_with_btc();
    WirePositionRisk wire{};
    wire.symbol = "BTCUSDT";
    wire.position_amt = "-0.015";
    wire.entry_price = "64123.47";
    const auto report = map_position(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, -15);
    EXPECT_GT(report->avg_entry.raw, 0);
}

TEST(BinanceMap, UserTradeBecomesAFillReport) {
    const auto table = table_with_btc();
    WireTrade wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "64488.60";
    wire.qty = "0.004";
    wire.commission = "0.01289772";
    wire.side = "SELL";
    wire.trade_id = 698759;
    wire.order_id = 25851813;
    wire.time_ms = 1'786'375'246'658ULL;
    wire.maker = true;
    const auto report = map_user_trade(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->side, Side::Sell);
    EXPECT_EQ(report->qty.raw, 4);
    EXPECT_EQ(report->liquidity, Liquidity::Maker);
    EXPECT_EQ(report->exec_id.view(), "698759");
}

// Order book -------------------------------------------------------------
//
// The three depth fixtures are one capture: `rest_depth_seed.json` is the
// snapshot, `ws_depth_straddle.json` is the delta whose [U, u] contains its
// `lastUpdateId`, and `ws_depth_straddle_next.json` is the frame after that.
// A seed and a stream from different minutes would replay into a crossed
// book, which is exactly the failure these tests exist to catch.

// The parsed views point into the frame text and into the level buffer, so
// both have to outlive the wire struct that came out of them.
struct Frame {
    std::string text;
    std::unique_ptr<LevelBuffer> storage{std::make_unique<LevelBuffer>()};
};

DepthBook book_for(const exec::InstrumentTable& table) {
    const auto* instrument = table.find("BTCUSDT");
    EXPECT_NE(instrument, nullptr);
    const auto& base = base_of(*instrument);
    return DepthBook{base.id, base.tick_size, base.lot_size};
}

WireDepthUpdate delta_of(Frame& frame, std::string_view name) {
    frame.text = fixture(name);
    const auto data = unwrap(frame.text);
    EXPECT_TRUE(data.has_value());
    const auto wire = parse_depth_update(data.value_or(std::string_view{}), *frame.storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireDepthUpdate{});
}

WireDepthSnapshot seed_of(Frame& frame) {
    frame.text = fixture("rest_depth_seed.json");
    const auto wire = parse_depth_snapshot(frame.text, *frame.storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireDepthSnapshot{});
}

// Bids descend, asks ascend, the two never cross and every quantity is real.
// A book that fails this is not a book, and publishing it would price every
// strategy reading shared memory.
void expect_coherent(const BookSnapshot& out) {
    ASSERT_GT(out.bid_count, 0U);
    ASSERT_GT(out.ask_count, 0U);
    EXPECT_LE(out.bid_count, k_book_levels);
    EXPECT_LE(out.ask_count, k_book_levels);
    EXPECT_LT(out.bids[0].price.raw, out.asks[0].price.raw);
    for (std::uint32_t i = 1; i < out.bid_count; ++i) {
        EXPECT_LT(out.bids[i].price.raw, out.bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].price.raw, out.asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < out.bid_count; ++i) {
        EXPECT_GT(out.bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].qty.raw, 0) << i;
    }
}

TEST(BinanceBook, AnUnseededBookPublishesNothing) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    Frame first;
    const auto delta = delta_of(first, "ws_depth_straddle.json");
    EXPECT_TRUE(book.wants_seed());
    EXPECT_EQ(book.apply(delta), BookResult::Buffered);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
}

TEST(BinanceBook, SeedAndStreamMeetInABookSharedMemoryCanTake) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    Frame seed_frame;
    Frame first;
    Frame second;
    const auto snapshot = seed_of(seed_frame);
    // Nothing is buffered yet, so the seed can only wait to be straddled.
    EXPECT_EQ(book.seed(snapshot), BookResult::Waiting);
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());

    const auto d1 = delta_of(first, "ws_depth_straddle.json");
    ASSERT_LE(d1.first_id, snapshot.last_update_id);
    ASSERT_GE(d1.last_id, snapshot.last_update_id);
    EXPECT_EQ(book.apply(d1), BookResult::Applied);

    const auto d2 = delta_of(second, "ws_depth_straddle_next.json");
    EXPECT_EQ(d2.prev_id, d1.last_id) << "futures chain: pu is the previous u";
    EXPECT_NE(d2.first_id, d1.last_id + 1) << "the spot rule would reject this frame";
    EXPECT_EQ(book.apply(d2), BookResult::Applied);

    const auto out = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(out->venue_seq, d2.last_id) << "u is the forensic handle on this book";
    EXPECT_EQ(out->ts_init.ns, k_ingest.ns);
}

TEST(BinanceBook, DeltasBufferedBehindTheSeedReplayIntoTheSameBook) {
    // Which of the REST answer and the websocket frame lands first is a race,
    // and the book it produces must not depend on who won.
    const auto table = table_with_btc();
    Frame seed_frame;
    Frame first;
    Frame second;
    const auto snapshot = seed_of(seed_frame);
    const auto d1 = delta_of(first, "ws_depth_straddle.json");
    const auto d2 = delta_of(second, "ws_depth_straddle_next.json");

    auto streamed = book_for(table);
    EXPECT_EQ(streamed.seed(snapshot), BookResult::Waiting);
    EXPECT_EQ(streamed.apply(d1), BookResult::Applied);
    EXPECT_EQ(streamed.apply(d2), BookResult::Applied);

    auto buffered = book_for(table);
    EXPECT_EQ(buffered.apply(d1), BookResult::Buffered);
    EXPECT_EQ(buffered.apply(d2), BookResult::Buffered);
    EXPECT_EQ(buffered.seed(snapshot), BookResult::Applied);

    const auto from_stream = streamed.snapshot(k_ingest, k_ingest);
    const auto from_buffer = buffered.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(from_stream.has_value());
    ASSERT_TRUE(from_buffer.has_value());
    expect_coherent(*from_buffer);
    EXPECT_EQ(from_buffer->venue_seq, from_stream->venue_seq);
    for (std::uint32_t i = 0; i < k_book_levels; ++i) {
        EXPECT_EQ(from_buffer->bids[i].price.raw, from_stream->bids[i].price.raw) << i;
        EXPECT_EQ(from_buffer->bids[i].qty.raw, from_stream->bids[i].qty.raw) << i;
        EXPECT_EQ(from_buffer->asks[i].price.raw, from_stream->asks[i].price.raw) << i;
        EXPECT_EQ(from_buffer->asks[i].qty.raw, from_stream->asks[i].qty.raw) << i;
    }
}

TEST(BinanceBook, AGapInTheChainPublishesNothing) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    Frame seed_frame;
    Frame first;
    Frame second;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    EXPECT_EQ(book.apply(delta_of(first, "ws_depth_straddle.json")), BookResult::Applied);

    auto skipped = delta_of(second, "ws_depth_straddle_next.json");
    skipped.prev_id += 1; // one event of the chain went missing

    EXPECT_EQ(book.apply(skipped), BookResult::Gap);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_TRUE(book.wants_seed()) << "a dropped book asks for a new seed";
}

TEST(BinanceBook, ZeroSizeDeletesTheLevel) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    Frame seed_frame;
    Frame first;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    const auto d1 = delta_of(first, "ws_depth_straddle.json");
    EXPECT_EQ(book.apply(d1), BookResult::Applied);

    const auto before = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->bids[0].price.raw, 631'228) << "63122.80 over a 0.10 tick";
    const auto next_down = before->bids[1].price.raw;

    const std::array<WireLevel, 1> gone{WireLevel{.price = "63122.80", .qty = "0"}};
    WireDepthUpdate delete_top{};
    delete_top.symbol = "BTCUSDT";
    delete_top.prev_id = d1.last_id;
    delete_top.first_id = d1.last_id + 1;
    delete_top.last_id = d1.last_id + 2;
    delete_top.bids = gone;
    EXPECT_EQ(book.apply(delete_top), BookResult::Applied);

    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->bids[0].price.raw, next_down) << "a zero deletes, it does not rest at zero";
    EXPECT_EQ(after->bids[0].qty.raw, before->bids[1].qty.raw);
}

TEST(BinanceBook, ALevelOffTheTickIsDroppedNotRounded) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    Frame seed_frame;
    Frame first;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    const auto d1 = delta_of(first, "ws_depth_straddle.json");
    EXPECT_EQ(book.apply(d1), BookResult::Applied);
    const auto depth_before = book.bids().size();
    const auto dropped_before = book.dropped_levels();
    const auto before = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(before.has_value());

    // 0.05 past a 0.10 tick: rounding it would invent a price binance never
    // quoted, one tick from the one it meant.
    const std::array<WireLevel, 1> off{WireLevel{.price = "63122.85", .qty = "1.000"}};
    WireDepthUpdate bad{};
    bad.symbol = "BTCUSDT";
    bad.prev_id = d1.last_id;
    bad.first_id = d1.last_id + 1;
    bad.last_id = d1.last_id + 2;
    bad.bids = off;
    EXPECT_EQ(book.apply(bad), BookResult::Applied);

    EXPECT_EQ(book.bids().size(), depth_before);
    EXPECT_EQ(book.dropped_levels(), dropped_before + 1);
    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    for (std::uint32_t i = 0; i < k_book_levels; ++i) {
        EXPECT_EQ(after->bids[i].price.raw, before->bids[i].price.raw) << i;
        EXPECT_EQ(after->bids[i].qty.raw, before->bids[i].qty.raw) << i;
    }
}

TEST(BinanceBook, ASeedNothingStraddlesIsDiscardedRatherThanTrusted) {
    // The ids are not dense per symbol, so a snapshot can land in the hole
    // between two frames. Trusting it would splice two different books.
    const auto table = table_with_btc();
    auto book = book_for(table);
    Frame seed_frame;
    Frame first;
    auto snapshot = seed_of(seed_frame);
    const auto d1 = delta_of(first, "ws_depth_straddle.json");
    snapshot.last_update_id = d1.first_id - 1;

    EXPECT_EQ(book.seed(snapshot), BookResult::Waiting);
    EXPECT_EQ(book.apply(d1), BookResult::Reseed);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_TRUE(book.wants_seed());
}

// ------------------------------------------------------------ balances ----

TEST(BinanceBook, SeedAnswersAreClassified) {
    constexpr std::string_view invalid = R"({"code":-1121,"msg":"Invalid symbol."})";
    EXPECT_EQ(classify_seed_answer(400, invalid), SeedAnswer::NoBook);
    EXPECT_EQ(classify_seed_answer(400, "<html>Bad Request</html>"), SeedAnswer::NoBook);
    EXPECT_EQ(classify_seed_answer(429, R"({"code":-1003,"msg":"Too many requests."})"),
              SeedAnswer::RateLimited);
    EXPECT_EQ(classify_seed_answer(418, R"({"code":-1003,"msg":"banned until 1"})"),
              SeedAnswer::RateLimited);
    EXPECT_EQ(classify_seed_answer(400, R"({"code":-1003,"msg":"Too many requests."})"),
              SeedAnswer::RateLimited);
    EXPECT_EQ(classify_seed_answer(400, R"({"code":-1001,"msg":"Disconnected."})"),
              SeedAnswer::Retry);
    EXPECT_EQ(classify_seed_answer(503, ""), SeedAnswer::Retry);
    EXPECT_EQ(classify_seed_answer(408, ""), SeedAnswer::Retry);
    EXPECT_EQ(classify_seed_answer(200, R"({"lastUpdateId":1,"bids":[],"asks":[]})"),
              SeedAnswer::Book);
}

struct FakeRest {
    int status{0};
    std::string body;
    int asked{0};
};

// Every delta asks for a seed while the book wants one, as the data client does.
void deliver_deltas(DepthBook& book, FakeRest& rest, int deltas) {
    for (int i = 0; i < deltas; ++i) {
        if (!book.wants_seed()) {
            continue;
        }
        book.seed_requested();
        ++rest.asked;
        if (classify_seed_answer(rest.status, rest.body) == SeedAnswer::NoBook) {
            book.seed_refused();
        } else {
            book.seed_failed();
        }
    }
}

TEST(BinanceBook, ASymbolTheVenueHasNoBookForIsAskedOncePerCatalogue) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    FakeRest rest{.status = 400, .body = R"({"code":-1121,"msg":"Invalid symbol."})", .asked = 0};
    deliver_deltas(book, rest, 100);
    EXPECT_EQ(rest.asked, 1);
    EXPECT_TRUE(book.refused());
    EXPECT_FALSE(book.live());

    book.readmit();
    deliver_deltas(book, rest, 100);
    EXPECT_EQ(rest.asked, 2);
}

TEST(BinanceBook, ARateLimitedSeedIsAskedAgain) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    FakeRest rest{.status = 429, .body = R"({"code":-1003,"msg":"Too many requests."})", .asked = 0};
    deliver_deltas(book, rest, 5);
    EXPECT_EQ(rest.asked, 5);
    EXPECT_FALSE(book.refused());
    EXPECT_TRUE(book.wants_seed());
}

TEST(BinanceMap, BalanceKeepsOnlyTheAssetsWithMoneyInThem) {
    auto storage = std::make_unique<BalanceBuffer>();
    const auto body = fixture("rest_account.json");
    const auto account = parse_account(body, *storage);
    ASSERT_TRUE(account.has_value());
    ASSERT_EQ(account->assets.size(), 20U);
    // Single-asset mode, and the rows below are readable only because of it.
    EXPECT_FALSE(account->multi_assets_margin);

    std::vector<BalanceReport> held;
    for (const auto& row : account->assets) {
        if (const auto report = map_balance(row, account->multi_assets_margin);
            report.has_value()) {
            held.push_back(*report);
        }
    }
    // Eighteen of the twenty rows are binance naming an asset it WOULD take as
    // collateral. The snapshot that carries these holds 64 rows, so forwarding
    // them costs the balances the account actually has.
    ASSERT_EQ(held.size(), 2U);
    EXPECT_EQ(held[0].asset.view(), "USDT");
    EXPECT_EQ(held[0].total.raw, 12'425'260'592'170);
    // Published, because the account margins each asset out of itself here:
    // 11982.40 free against 12425.26 held, the difference being the margin the
    // open position already committed.
    EXPECT_EQ(held[0].available.raw, 11'982'402'392'170);
    EXPECT_LT(held[0].available.raw, held[0].total.raw);
    EXPECT_EQ(held[0].unrealized.raw, -18'429'100'000);
    EXPECT_EQ(held[0].ts_venue.ns, 1'787'841'486'006'000'000ULL);
    // Flat unrealised pnl does not make a row empty: the BNB is held.
    EXPECT_EQ(held[1].asset.view(), "BNB");
    EXPECT_EQ(held[1].total.raw, 1'550'100'000);
    EXPECT_EQ(held[1].unrealized.raw, 0);
    // And the free figure is this asset's own, not the USDT one repeated.
    EXPECT_EQ(held[1].available.raw, 1'550'100'000);
}

TEST(BinanceMap, BalanceWithholdsAvailableInMultiAssetsMode) {
    // The failure this endpoint was chosen to prevent. In multi-assets mode
    // binance repeats ONE account-wide margin on every row, so an account
    // holding 827 USDT answers ~826 as the "available" of BTC as well —
    // published per asset that is 826 free BTC.
    const std::string body =
        R"({"multiAssetsMargin":true,"assets":[)"
        R"({"asset":"USDT","walletBalance":"827.00000000","unrealizedProfit":"0.00000000",)"
        R"("availableBalance":"826.00000000","updateTime":1787841486006},)"
        R"({"asset":"BTC","walletBalance":"0.01000000","unrealizedProfit":"0.00000000",)"
        R"("availableBalance":"826.00000000","updateTime":1787841486006}],"positions":[]})";
    auto storage = std::make_unique<BalanceBuffer>();
    const auto account = parse_account(body, *storage);
    ASSERT_TRUE(account.has_value());
    ASSERT_TRUE(account->multi_assets_margin);
    ASSERT_EQ(account->assets.size(), 2U);

    for (const auto& row : account->assets) {
        const auto report = map_balance(row, account->multi_assets_margin);
        ASSERT_TRUE(report.has_value());
        // Undef, not 826 and not zero: the venue gave no per-asset free
        // figure, and undef is what says so.
        EXPECT_TRUE(report->available.is_undef()) << report->asset.view();
        // The total and the pnl are per asset in either mode, so withholding
        // the one figure must not withhold the other two.
        EXPECT_FALSE(report->total.is_undef()) << report->asset.view();
    }
}

TEST(BinanceMap, AModeBinanceDidNotStateWithholdsTheAvailable) {
    // v3 of this endpoint already drops `multiAssetsMargin`. A reader that
    // took an absent flag for `false` would publish one account-wide margin as
    // the free balance of every asset — so absence withholds instead.
    const std::string body = R"({"assets":[{"asset":"USDT","walletBalance":"827.00000000",)"
                             R"("unrealizedProfit":"0.00000000","availableBalance":"826.00000000",)"
                             R"("updateTime":1787841486006}],"positions":[]})";
    auto storage = std::make_unique<BalanceBuffer>();
    const auto account = parse_account(body, *storage);
    ASSERT_TRUE(account.has_value());
    EXPECT_TRUE(account->multi_assets_margin);
    ASSERT_EQ(account->assets.size(), 1U);
    const auto report = map_balance(account->assets[0], account->multi_assets_margin);
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report->available.is_undef());
}

TEST(BinanceMap, BalanceIsDroppedOnlyWhenTheWalletWasReportedAndIsEmpty) {
    WireAssetBalance wire{};
    wire.asset = "USDC";
    wire.wallet_balance = "0.00000000";
    wire.available_balance = "826.00000000";
    wire.unrealized_profit = "0.00000000";
    // Binance names every asset it would accept as collateral. An empty wallet
    // with no pnl on it is one of those, not a holding.
    EXPECT_FALSE(map_balance(wire, false).has_value());

    // Unrealised pnl on an empty wallet IS a holding: the position is open.
    wire.unrealized_profit = "-1.50000000";
    EXPECT_TRUE(map_balance(wire, false).has_value());

    // A wallet figure binance did not send is not an empty wallet. Reading it
    // as one would drop a balance nobody was told about.
    wire.unrealized_profit = "0.00000000";
    wire.wallet_balance = {};
    EXPECT_TRUE(map_balance(wire, false).has_value());
}

TEST(BinanceMap, BalanceWithoutAnAssetIsDropped) {
    // A wallet figure with no unit is not a balance, and a truncated ticker
    // would attribute the money to an asset that does not exist.
    WireAssetBalance wire{};
    wire.wallet_balance = "12.50000000";
    EXPECT_FALSE(map_balance(wire, false).has_value());
    wire.asset = "TWENTYONECHARACTERSXX";
    EXPECT_FALSE(map_balance(wire, false).has_value());
}

TEST(BinanceMap, BalanceLeavesAFieldBinanceDidNotSendUndef) {
    WireAssetBalance wire{};
    wire.asset = "USDT";
    wire.wallet_balance = "1200.00000000";
    const auto report = map_balance(wire, false);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->total.raw, 1'200'000'000'000);
    // Not zero, and never the total: repeating the total into `available`
    // invites a caller to spend margin that is already committed.
    EXPECT_TRUE(report->available.is_undef());
    EXPECT_TRUE(report->unrealized.is_undef());
}

TEST(BinanceMap, BalanceReadsMorePrecisionThanItStores) {
    // Not what binance sends today — eight fractional digits is. The exact
    // reader refuses a number past nine outright, and one refused balance is
    // an account reading as empty, so the wallet reader truncates instead.
    WireAssetBalance wire{};
    wire.asset = "USDT";
    wire.wallet_balance = "1200.123456789012";
    const auto report = map_balance(wire, false);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->total.raw, 1'200'123'456'789);
}

[[nodiscard]] InstrumentBase classified(std::string_view symbol,
                                        std::string_view base_asset,
                                        std::string_view contract_type,
                                        std::string_view underlying_type) {
    WireInstrument wire{};
    wire.symbol = symbol;
    wire.base_asset = base_asset;
    wire.quote_asset = "USDT";
    wire.contract_type = contract_type;
    wire.underlying_type = underlying_type;
    wire.status = "TRADING";
    wire.tick_size = "0.01";
    wire.step_size = "0.001";
    wire.min_notional = "5";
    auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    if (!instrument.has_value()) {
        return {};
    }
    classify(VenueId::Binance, *instrument);
    return base_of(*instrument);
}

TEST(BinanceMap, UnderlyingTypeSetsTheAssetClass) {
    const auto tsla = classified("TSLAUSDT", "TSLA", "TRADIFI_PERPETUAL", "EQUITY");
    EXPECT_EQ(tsla.asset_class, AssetClass::Equity);
    EXPECT_EQ(tsla.underlying.view(), "TSLA");

    const auto xau = classified("XAUUSDT", "XAU", "TRADIFI_PERPETUAL", "COMMODITY");
    EXPECT_EQ(xau.asset_class, AssetClass::Commodity);
    EXPECT_EQ(xau.underlying.view(), "XAU");

    const auto spx = classified("SPXUSDT", "SPX", "PERPETUAL", "COIN");
    EXPECT_EQ(spx.asset_class, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");

    EXPECT_EQ(classified("XYZUSDT", "XYZ", "PERPETUAL", "NEWTYPE").asset_class, AssetClass::Unknown);
}

} // namespace
} // namespace raikiri::venues::binance
