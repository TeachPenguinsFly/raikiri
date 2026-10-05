#include <raikiri/domain/asset_class.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/bitget/account.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/bitget/book.hpp>
#include <raikiri/venues/bitget/catalogue.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/market_data.hpp>
#include <raikiri/venues/bitget/orders.hpp>
#include <raikiri/venues/bitget/positions.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace raikiri::venues::bitget {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

// bitget's deepest snapshot depth; books50 and books100 are refused
constexpr std::uint32_t k_bitget_depth = 15;

// rest_contracts.json mapped; PEPEUSDT is refused, leaving three
exec::InstrumentTable table_from_catalogue() {
    exec::InstrumentTable table;
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts.json");
    const auto rows = parse_contracts(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (const auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

const InstrumentBase* base_for(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

TEST(BitgetWire, EventDispatchResolvesKindAndInstrument) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker.json", EventKind::Ticker},
                                     std::pair{"ws_trade.json", EventKind::Trade},
                                     std::pair{"ws_books15.json", EventKind::Books},
                                     std::pair{"ws_books_update.json", EventKind::Books}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        // only arg names the instrument on every channel
        EXPECT_EQ(event->inst_id, "BTCUSDT") << file;
    }
}

TEST(BitgetWire, ControlFramesAreNotMistakenForData) {
    const auto ack = fixture("ws_subscribe_ack.json");
    auto event = parse_event(ack);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::SubscribeAck);

    const auto bad = fixture("ws_error_unknown_instrument.json");
    event = parse_event(bad);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Error);
}

TEST(BitgetWire, StreamErrorCodeIsANumberWhileTheRestCodeIsAString) {
    // the ws code is a number and the rest code a string
    const auto bad_instrument = fixture("ws_error_unknown_instrument.json");
    auto err = parse_stream_error(bad_instrument);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_ws_unknown_subscription);
    EXPECT_FALSE(err->msg.empty());

    const auto refusal = fixture("rest_error_unknown_symbol.json");
    const auto envelope = parse_rest_envelope(refusal);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, "40034");
}

TEST(BitgetWire, KeepaliveIsExactlyFourBytesAndItsAnswerIsNotJson) {
    // the bare pong is ignored like any unknown frame
    EXPECT_FALSE(parse_event("pong").has_value());
    EXPECT_EQ(Protocol::ping_frame(), "ping");

    // a trailing newline is refused; the parser returns the escaped text
    const auto refused = fixture("ws_error_unrecognized_request.json");
    const auto err = parse_stream_error(refused);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_ws_unrecognized_request);
    EXPECT_EQ(err->msg, R"(Unrecognized request:ping\n)");
}

TEST(BitgetWire, TickerCarriesBookMarkAndFundingOnOneFrame) {
    const auto frame = fixture("ws_ticker.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->inst_id, "BTCUSDT");
    EXPECT_EQ(ticker->bid_price, "63923.8");
    EXPECT_EQ(ticker->ask_price, "63923.9");
    EXPECT_EQ(ticker->bid_qty, "11.3297");
    EXPECT_EQ(ticker->ask_qty, "15.7309");
    EXPECT_EQ(ticker->mark_price, "63923.8");
    EXPECT_EQ(ticker->index_price, "63936.84");
    EXPECT_EQ(ticker->funding_rate, "0.0001");
    // base and quote turnover of the same window
    EXPECT_EQ(ticker->volume_24h_base, "30602.4556");
    EXPECT_EQ(ticker->volume_24h_quote, "1969900863.29574");
    EXPECT_EQ(ticker->next_funding_ms, 1'786'435'200'000ULL);
    // the element's ts, not the frame's outer one
    EXPECT_EQ(ticker->ts_ms, 1'786'430'817'946ULL);

    // views point into the caller's buffer, not parser scratch
    EXPECT_GE(ticker->bid_price.data(), frame.data());
    EXPECT_LE(ticker->bid_price.data() + ticker->bid_price.size(), frame.data() + frame.size());
}

TEST(BitgetWire, TradesCarryTakerSideDirectly) {
    auto storage = std::make_unique<TradeBuffer>();
    const auto frame = fixture("ws_trade.json");
    const auto trades = parse_trades(frame, *storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 4U); // one frame, four prints
    const auto& trade = trades->front();
    EXPECT_EQ(trade.inst_id, "BTCUSDT"); // supplied by arg; the element has none
    EXPECT_EQ(trade.trade_id, "1470962960790470662");
    EXPECT_EQ(trade.price, "63923.9");
    EXPECT_EQ(trade.qty, "0.0866");
    EXPECT_EQ(trade.ts_ms, 1'786'430'819'699ULL);
    // "buy" already means the aggressor bought
    EXPECT_TRUE(trade.aggressor_bought);
}

TEST(BitgetWire, BooksChannelsShareOneShapeAndChainOnSeq) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_frame = fixture("ws_books15.json");
    const auto snapshot = parse_books(snapshot_frame, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    EXPECT_EQ(snapshot->bids.size(), k_bitget_depth);
    EXPECT_EQ(snapshot->asks.size(), k_bitget_depth);
    // books15 pins pseq to 0
    EXPECT_EQ(snapshot->prev_seq_id, 0);
    EXPECT_EQ(snapshot->seq_id, 830'988'172'443);

    // the books channel ships the same element, so one parser reads both
    auto delta_storage = std::make_unique<LevelBuffer>();
    const auto delta_frame = fixture("ws_books_update.json");
    const auto delta = parse_books(delta_frame, *delta_storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->snapshot);
    EXPECT_GT(delta->prev_seq_id, 0);
    EXPECT_GT(delta->seq_id, delta->prev_seq_id);
}

TEST(BitgetWire, DeletedLevelsKeepTheirZeroQuantityAndEmptySidesStayEmpty) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books_update.json");
    const auto books = parse_books(frame, *storage);
    ASSERT_TRUE(books.has_value());
    bool saw_delete = false;
    for (const auto& level : books->bids) {
        EXPECT_FALSE(level.price.empty());
        EXPECT_FALSE(level.qty.empty()); // "0" is a delete, never an absence
        saw_delete = saw_delete || level.qty == "0";
    }
    EXPECT_TRUE(saw_delete);

    // a one-sided delta is a valid frame
    auto one_sided_storage = std::make_unique<LevelBuffer>();
    const auto one_sided_frame = fixture("ws_books_update_one_sided.json");
    const auto one_sided = parse_books(one_sided_frame, *one_sided_storage);
    ASSERT_TRUE(one_sided.has_value());
    EXPECT_TRUE(one_sided->bids.empty());
    EXPECT_EQ(one_sided->asks.size(), 1U);
}

TEST(BitgetWire, RestEnvelopeSucceedsOnTheStringNotTheNumber) {
    for (const char* file : {"rest_contracts.json",
                             "rest_ticker.json",
                             "rest_orderbook.json",
                             "rest_current_fund_rate.json",
                             "rest_server_time.json"}) {
        const auto body = fixture(file);
        const auto envelope = parse_rest_envelope(body);
        ASSERT_TRUE(envelope.has_value()) << file;
        EXPECT_EQ(envelope->code, k_ok) << file;
        // five characters; integer conversion would collapse it to zero
        EXPECT_EQ(envelope->code, "00000") << file;
    }

    const auto missing_product = fixture("rest_error_missing_product_type.json");
    const auto envelope = parse_rest_envelope(missing_product);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, k_parameter_check_failed);
    EXPECT_NE(envelope->code, k_ok);
}

TEST(BitgetWire, UnreadableEnvelopeIsNotReadAsSuccess) {
    EXPECT_FALSE(parse_rest_envelope(R"({"msg":"","data":null})").has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"code":null,"msg":"","data":null})").has_value());
    // A numerically-zero code is still not "00000".
    const std::string_view zero{R"({"code":"0","msg":"","data":null})"};
    const auto envelope = parse_rest_envelope(zero);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_NE(envelope->code, k_ok);
}

TEST(BitgetWire, ContractsReportWhatTheVenueSaid) {
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts.json");
    const auto rows = parse_contracts(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    const auto find = [&](std::string_view symbol) -> const WireContract* {
        for (const auto& row : *rows) {
            if (row.symbol == symbol) {
                return &row;
            }
        }
        return nullptr;
    };

    const auto* btc = find("BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->base_coin, "BTC");
    EXPECT_EQ(btc->quote_coin, "USDT");
    EXPECT_EQ(btc->symbol_type, "perpetual");
    EXPECT_EQ(btc->symbol_status, "normal");
    EXPECT_EQ(btc->price_place, "1");
    EXPECT_EQ(btc->price_end_step, "1");
    EXPECT_EQ(btc->size_multiplier, "0.0001");
    EXPECT_EQ(btc->fund_interval, "8");

    // the wire layer reports; the mapper refuses unusable rows
    const auto* shib = find("SHIBUSDT");
    ASSERT_NE(shib, nullptr);
    EXPECT_EQ(shib->price_place, "9");
    EXPECT_EQ(shib->size_multiplier, "10000");
    EXPECT_EQ(shib->min_trade_num, "10000");
    const auto* pepe = find("PEPEUSDT");
    ASSERT_NE(pepe, nullptr);
    EXPECT_EQ(pepe->price_place, "10");
}

TEST(BitgetWire, ServerTimeAndFundRateParse) {
    // every parsed view points into the caller's buffer
    const auto time_body = fixture("rest_server_time.json");
    const auto time = parse_server_time(time_body);
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(*time, 1'786'430'803'575ULL);

    const auto funding_body = fixture("rest_current_fund_rate.json");
    const auto funding = parse_current_fund_rate(funding_body);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->symbol, "BTCUSDT");
    EXPECT_EQ(funding->funding_rate, "0.0001");
    // this endpoint names the interval fundingRateInterval and the settlement nextUpdate
    EXPECT_EQ(funding->interval_hours, "8");
    EXPECT_EQ(funding->next_update_ms, 1'786'435'200'000ULL);
    EXPECT_EQ(funding->min_rate, "-0.003");
}

TEST(BitgetWire, RestShapesAreNotWebsocketShapes) {
    auto storage = std::make_unique<LevelBuffer>();

    // orderbook data is an object, so the ws reader must refuse it
    const auto book_body = fixture("rest_orderbook.json");
    const auto book = parse_rest_orderbook(book_body, *storage);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bids.size(), 5U);
    EXPECT_EQ(book->asks.size(), 5U);
    EXPECT_EQ(book->ts_ms, 1'786'430'802'539ULL);
    auto ws_storage = std::make_unique<LevelBuffer>();
    EXPECT_FALSE(parse_books(book_body, *ws_storage).has_value());

    // the rest ticker names the instrument symbol and omits nextFundingTime
    const auto ticker_body = fixture("rest_ticker.json");
    EXPECT_FALSE(parse_ticker(ticker_body).has_value());
}

TEST(BitgetWire, GarbageIsRefusedRatherThanPartiallyRead) {
    auto levels = std::make_unique<LevelBuffer>();
    auto trades = std::make_unique<TradeBuffer>();
    auto contracts = std::make_unique<ContractBuffer>();
    for (const char* junk : {"",
                             "{",
                             "[]",
                             "null",
                             R"({"arg":)",
                             "\xff\xfe",
                             R"({"arg":{"channel":"ticker"}})",
                             R"({"arg":{"channel":"ticker","instId":"BTCUSDT"},"data":[{}]})"}) {
        const std::string_view frame{junk};
        (void)parse_event(frame);
        EXPECT_FALSE(parse_ticker(frame).has_value()) << junk;
        EXPECT_FALSE(parse_trades(frame, *trades).has_value()) << junk;
        EXPECT_FALSE(parse_books(frame, *levels).has_value()) << junk;
        EXPECT_FALSE(parse_contracts(frame, *contracts).has_value()) << junk;
        EXPECT_FALSE(parse_server_time(frame).has_value()) << junk;
        EXPECT_FALSE(parse_current_fund_rate(frame).has_value()) << junk;
    }
    // an empty data array must not become a zero reading
    EXPECT_FALSE(parse_current_fund_rate(R"({"code":"40034","msg":"x","data":[]})").has_value());
}

TEST(BitgetMap, TickComposesPricePlaceWithPriceEndStep) {
    // only a constructed row tells priceEndStep * 10^-pricePlace apart from 10^-pricePlace
    WireContract wire{};
    wire.symbol = "FAKEUSDT";
    wire.base_coin = "FAKE";
    wire.quote_coin = "USDT";
    wire.symbol_type = "perpetual";
    wire.symbol_status = "normal";
    wire.size_multiplier = "1";
    wire.fund_interval = "8";
    wire.price_place = "2";
    wire.price_end_step = "5";
    auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).tick_size.raw, 50'000'000); // 0.05, not 0.01

    // pricePlace 0 is valid, not missing
    wire.price_place = "0";
    wire.price_end_step = "1";
    instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).tick_size.raw, k_e9); // 1

    // nine decimals is the most e9 fixed point holds
    wire.price_place = "9";
    EXPECT_TRUE(map_instrument(wire, MarketType::Perp).has_value());
    wire.price_place = "10";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
    // A zero or absent priceEndStep would make the tick zero too.
    wire.price_place = "2";
    wire.price_end_step = "0";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
    wire.price_end_step = "";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(BitgetMap, CatalogueRowsBecomeInstrumentsOrAreRefused) {
    const auto table = table_from_catalogue();
    // PEPEUSDT quotes ten price decimals and is refused
    EXPECT_EQ(table.size(), 3U);
    EXPECT_EQ(table.find("PEPEUSDT"), nullptr);

    const auto* btc = base_for(table, "BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->tick_size.raw, 100'000'000); // pricePlace 1 -> 0.1
    EXPECT_EQ(btc->lot_size.raw, 100'000);      // sizeMultiplier 0.0001
    EXPECT_EQ(btc->min_notional.raw, 5 * k_e9); // minTradeUSDT 5
    EXPECT_EQ(btc->contract_value.raw, k_e9);   // base units, not contracts
    EXPECT_EQ(btc->exchange_symbol.view(), "BTCUSDT");

    const auto* eth = base_for(table, "ETHUSDT");
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(eth->tick_size.raw, 10'000'000); // pricePlace 2 -> 0.01
    EXPECT_EQ(eth->lot_size.raw, 10'000'000);

    // SHIBUSDT separates sizeMultiplier from volumePlace
    const auto* shib = base_for(table, "SHIBUSDT");
    ASSERT_NE(shib, nullptr);
    EXPECT_EQ(shib->tick_size.raw, 1); // pricePlace 9 -> 0.000000001
    EXPECT_EQ(shib->lot_size.raw, 10'000 * k_e9);

    // fundInterval comes from the catalogue
    const auto* perp = std::get_if<PerpInstrument>(table.find("BTCUSDT"));
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(perp->funding_interval_s, 28'800U);
}

TEST(BitgetMap, LocalSymbolIsTheCanonicalPairOnBothCatalogues) {
    const auto table = table_from_catalogue();
    const auto* btc = base_for(table, "BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(btc->local_symbol.view(), "BTC/USDT");

    auto storage = std::make_unique<SpotSymbolBuffer>();
    const auto body = fixture("rest_spot_symbols.json");
    const auto rows = parse_spot_symbols(body, *storage);
    ASSERT_TRUE(rows.has_value());
    const auto spot = map_instrument((*rows)[0]);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(base_of(*spot).local_symbol.view(), "BTC/USDT");

    // a row naming no pair is refused
    auto unnamed = (*rows)[0];
    unnamed.quote_coin = {};
    EXPECT_FALSE(map_instrument(unnamed).has_value());
}

TEST(BitgetMap, UntradableAndNonPerpetualRowsAreRefused) {
    WireContract wire{};
    wire.symbol = "BTCUSDT";
    wire.base_coin = "BTC";
    wire.quote_coin = "USDT";
    wire.symbol_type = "perpetual";
    wire.symbol_status = "normal";
    wire.price_place = "1";
    wire.price_end_step = "1";
    wire.size_multiplier = "0.0001";
    wire.fund_interval = "8";
    ASSERT_TRUE(map_instrument(wire, MarketType::Perp).has_value());

    // A dated contract shares this endpoint on the coin-margined products.
    wire.symbol_type = "delivery";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
    wire.symbol_type = "perpetual";

    // non-normal rows still ship full precision fields
    for (const char* status : {"maintain", "limit_open", "restrictedAPI", "off", "listed"}) {
        wire.symbol_status = status;
        EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value()) << status;
    }
    wire.symbol_status = "normal";

    // spot uses a different endpoint and row shape
    EXPECT_FALSE(map_instrument(wire, MarketType::Spot).has_value());
    EXPECT_FALSE(map_instrument(wire, MarketType::Future).has_value());
}

TEST(BitgetMap, TickerBecomesTopOfBook) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.bid_price = "63923.8";
    wire.bid_qty = "11.3297";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    wire.ts_ms = 1'786'430'817'946ULL;

    const auto bbo = map_bbo(wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 639'238); // 63923.8 / 0.1
    EXPECT_EQ(bbo->ask.raw, 639'239);
    EXPECT_EQ(bbo->bid_qty.raw, 113'297); // 11.3297 / 0.0001
    EXPECT_EQ(bbo->ask_qty.raw, 157'309);
    EXPECT_EQ(bbo->ts_event.ns, 1'786'430'817'946ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(BitgetMap, BookSnapshotCrossesIntoTheDomain) {
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books15.json");
    const auto wire = parse_books(frame, *storage);
    ASSERT_TRUE(wire.has_value());

    const auto book = map_book(*wire, table, k_ingest);
    ASSERT_TRUE(book.has_value());
    // every subscribed level reaches the snapshot
    EXPECT_EQ(book->bid_count, k_bitget_depth);
    EXPECT_EQ(book->ask_count, k_bitget_depth);
    static_assert(k_bitget_depth <= k_book_levels);
    EXPECT_EQ(book->venue_seq, 830'988'172'443ULL);
    EXPECT_EQ(book->ts_event.ns, 1'786'983'217'049ULL * 1'000'000ULL);
    EXPECT_EQ(book->ts_init.ns, k_ingest.ns);

    // bids descend, asks ascend, and the book does not cross
    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw);
    for (std::uint32_t i = 1; i < book->bid_count; ++i) {
        EXPECT_LT(book->bids[i].price.raw, book->bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].price.raw, book->asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < book->bid_count; ++i) {
        EXPECT_GT(book->bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].qty.raw, 0) << i;
    }
}

TEST(BitgetMap, AnIncrementalFrameNeverBecomesABook) {
    // a delta published as a snapshot would leave holes in the book
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books_update.json");
    const auto wire = parse_books(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_FALSE(wire->snapshot);
    EXPECT_FALSE(map_book(*wire, table, k_ingest).has_value());
}

TEST(BitgetMap, AnUnknownInstrumentYieldsNoBook) {
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books15.json");
    auto wire = parse_books(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    wire->inst_id = "NOSUCHUSDT";
    EXPECT_FALSE(map_book(*wire, table, k_ingest).has_value());
}

TEST(BitgetMap, ValuesOffTheStepAreDroppedNotRounded) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.bid_price = "63923.85"; // 0.05 past a 0.1 tick
    wire.bid_qty = "11.3297";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());

    // an off-step quantity is refused like an off-tick price
    WireTrade trade{};
    trade.inst_id = "SHIBUSDT";
    trade.price = "0.000004496";
    trade.qty = "5000";
    EXPECT_FALSE(map_trade(trade, table, k_ingest).has_value());
    trade.qty = "2220000"; // a real captured print
    const auto mapped = map_trade(trade, table, k_ingest);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(mapped->qty.raw, 222);
}

TEST(BitgetMap, EmptySideAndUnknownInstrumentAreDropped) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    // an empty side must not become a zero price
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());

    wire.bid_price = "63923.8";
    wire.bid_qty = "11.3297";
    wire.inst_id = "PEPEUSDT"; // refused by the catalogue, so never in the table
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(BitgetMap, TakerSideIsCarriedThroughUnflipped) {
    // bitget states the aggressor's side; no inversion
    const auto table = table_from_catalogue();
    WireTrade wire{};
    wire.inst_id = "BTCUSDT";
    wire.price = "63923.9";
    wire.qty = "0.0866";
    wire.ts_ms = 1'786'430'819'699ULL;

    wire.aggressor_bought = true;
    auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 639'239);
    EXPECT_EQ(trade->qty.raw, 866);

    wire.aggressor_bought = false;
    trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
}

TEST(BitgetMap, MissingVenueTimeFallsBackToIngest) {
    const auto table = table_from_catalogue();
    WireTrade wire{};
    wire.inst_id = "BTCUSDT";
    wire.price = "63923.9";
    wire.qty = "0.0866";
    wire.ts_ms = 0;
    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // a zero timestamp leaves the trade unstamped
    EXPECT_EQ(trade->ts_event.ns, 0U);
}

TEST(BitgetMap, FundingRidesTheTickerFrame) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.bid_price = "63923.8";
    wire.bid_qty = "11.3297";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    wire.mark_price = "63923.8";
    wire.funding_rate = "0.0001";
    wire.next_funding_ms = 1'786'435'200'000ULL;
    wire.ts_ms = 1'786'430'817'946ULL;

    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 0.0001 * 1e9
    EXPECT_EQ(funding->mark_price.raw, 639'238);
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'435'200'000ULL * 1'000'000ULL);
    // from the catalogue
    EXPECT_EQ(funding->interval_s, 28'800U);
    // this channel has no forecast, so it stays undef
    EXPECT_TRUE(funding->predicted_rate.is_undef());

    // A contract quoting no rate is not a contract funding at zero.
    wire.funding_rate = "";
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

TEST(BitgetMap, TurnoverRidesTheSameTickerFrame) {
    const auto table = table_from_catalogue();
    const auto frame = fixture("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());

    const auto stats = map_ticker_stats(*wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 30'602'455'600'000LL);
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'969'900'863'295'740LL);
    EXPECT_EQ(stats->ts_event.ns, 1'786'430'817'946ULL * 1'000'000ULL);

    // an unknown instrument has no cell to write
    auto unknown = *wire;
    unknown.inst_id = "NOTLISTEDUSDT";
    EXPECT_FALSE(map_ticker_stats(unknown, table, k_ingest).has_value());

    // a frame with neither column publishes nothing
    auto silent = *wire;
    silent.volume_24h_base = {};
    silent.volume_24h_quote = {};
    EXPECT_FALSE(map_ticker_stats(silent, table, k_ingest).has_value());
}

TEST(BitgetWire, BillsSitUnderDataBillsBesideTheEndIdCursor) {
    auto storage = std::make_unique<BillBuffer>();
    const auto body = fixture("rest_mix_account_bill.json");
    const auto rows = parse_mix_bills(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].bill_id, "1204387519231889409");
    EXPECT_EQ((*rows)[0].symbol, "BTCUSDT");
    EXPECT_EQ((*rows)[0].business_type, "contract_settle_fee");
    EXPECT_EQ((*rows)[0].amount, "-0.004992");
    EXPECT_EQ((*rows)[0].coin, "USDT");
    // cTime is a quoted millisecond stamp; requestTime is unquoted
    EXPECT_EQ((*rows)[0].c_time_ms, 1'695'715'200'654ULL);
}

TEST(BitgetMap, AFundingPaymentAndAFundingReceiptDoNotBothComeOutPositive) {
    // amount is the signed balance move; its magnitude would book payments as income
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<BillBuffer>();
    const auto body = fixture("rest_mix_account_bill.json");
    const auto rows = parse_mix_bills(body, *storage);
    ASSERT_TRUE(rows.has_value());

    std::vector<LedgerEntry> booked;
    for (const auto& wire : *rows) {
        if (const auto charge = map_bill(wire, table, UnixNanos{7}); charge.has_value()) {
            booked.push_back(*charge);
        }
    }
    // open_long is a trade already booked by its fill; SOLUSDT is not in this catalogue
    ASSERT_EQ(booked.size(), 2U);

    EXPECT_EQ(booked[0].kind, LedgerKind::Funding);
    EXPECT_EQ(booked[0].amount.raw, -4'992'000) << "0.004992 paid away";
    EXPECT_EQ(booked[1].amount.raw, 12'810'000) << "0.01281 received, and NOT flipped";
    EXPECT_EQ(booked[0].settled_at.ns, 1'695'715'200'654ULL * 1'000'000ULL);
    // the venue row id tells settlements apart, not the stamp
    EXPECT_EQ(booked[0].reference.view(), "1204387519231889409");
    EXPECT_EQ(booked[0].ts_init.ns, 7U);
}

TEST(BitgetMap, ATradeRowIsRefusedEvenThoughTheQueryAsksForSettlementsOnly) {
    // the parser filters businessType itself rather than trusting the query parameter
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<BillBuffer>();
    const auto body = fixture("rest_mix_account_bill.json");
    const auto rows = parse_mix_bills(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ((*rows)[2].business_type, "open_long");
    EXPECT_FALSE(map_bill((*rows)[2], table, UnixNanos{7}).has_value());
}

TEST(BitgetRest, TheBillReadNarrowsAtTheVenueAndCarriesTheCursorItWasGiven) {
    net::Signer signer;
    signer.set_api_key("bitget-test-key");
    ASSERT_TRUE(signer.secret().assign("bitget-test-secret").has_value());
    RequestBuilder builder;
    builder.set_credentials(signer, "bitget-test-passphrase");

    constexpr UnixNanos now{1'700'000'000'000'000'000ULL};
    const auto since = UnixNanos{1'695'715'200'654ULL * 1'000'000ULL};
    const auto request = builder.settle_bills(MarketType::Perp, product_type(false), since, now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    // keys ascend; the signature covers the query as sent
    EXPECT_EQ(request->target,
              "/api/v2/mix/account/bill?businessType=contract_settle_fee&limit=100"
              "&productType=USDT-FUTURES&startTime=1695715200654");

    // a zero cursor sends no window; bitget refuses one wider than 30 days
    const auto from_zero = builder.settle_bills(MarketType::Perp, product_type(false), UnixNanos{0}, now);
    ASSERT_TRUE(from_zero.has_value());
    EXPECT_EQ(from_zero->target,
              "/api/v2/mix/account/bill?businessType=contract_settle_fee&limit=100"
              "&productType=USDT-FUTURES");

    // spot bills are another row shape
    for (const auto market : {MarketType::Spot, MarketType::Future, MarketType::Option}) {
        EXPECT_EQ(builder.settle_bills(market, product_type(false), since, now).error(), BuildError::UnsupportedMarket);
    }
}

TEST(BitgetErrors, ClockSkewIsRetryableAndUnknownCodesAreTerminal) {
    // a resync and retry succeeds, so skew is retryable
    EXPECT_EQ(classify(k_timestamp_expired), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_system_error), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    for (const auto& code :
         {k_access_key_empty, k_secret_key_empty, k_signature_empty, k_invalid_access_key, k_signature_error}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::AuthFailure) << code;
    }
    // unknown codes default to terminal
    for (const auto& code : {k_parameter_not_exist,
                             k_url_not_found,
                             k_parameter_check_failed,
                             std::string_view{"99999"},
                             std::string_view{"not-a-code"},
                             std::string_view{}}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::Terminal) << code;
    }
}

TEST(BitgetErrors, CodeNumberIsForReportingOnly) {
    EXPECT_EQ(code_number("40034"), 40034);
    EXPECT_EQ(code_number("400172"), 400172);
    // classification compares text, never this value
    EXPECT_EQ(code_number(k_ok), 0);
    EXPECT_EQ(code_number("not-a-code"), 0);
}

TEST(BitgetWs, SubscribeFrameCarriesInstType) {
    Protocol protocol;
    const std::array<Subscription, 1> one{
        Subscription{.inst_type = "USDT-FUTURES", .channel = "ticker", .inst_id = "BTCUSDT"}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 1U);
    // matches the frame the live socket acknowledged
    EXPECT_EQ(frame->text,
              R"({"op":"subscribe","args":[{"instType":"USDT-FUTURES","channel":"ticker","instId":"BTCUSDT"}]})");

    const std::array<Subscription, 2> two{
        Subscription{.inst_type = "USDT-FUTURES", .channel = "ticker", .inst_id = "BTCUSDT"},
        Subscription{.inst_type = "USDT-FUTURES", .channel = "trade", .inst_id = "ETHUSDT"}};
    const auto batched = protocol.subscribe_frame(two);
    ASSERT_TRUE(batched.has_value());
    EXPECT_EQ(batched->consumed, 2U); // batching, against a 10 msg/s budget
}

TEST(BitgetWs, IncompleteSubscriptionsAreRefusedRatherThanSent) {
    Protocol protocol;
    // instType is required; BTCUSDT exists under both live and demo products
    const std::array<Subscription, 1> no_type{Subscription{.inst_type = {}, .channel = "ticker", .inst_id = "BTCUSDT"}};
    EXPECT_FALSE(protocol.subscribe_frame(no_type).has_value());
    // an empty channel would earn a 30016 param error
    const std::array<Subscription, 1> no_channel{
        Subscription{.inst_type = "USDT-FUTURES", .channel = {}, .inst_id = "BTCUSDT"}};
    EXPECT_FALSE(protocol.subscribe_frame(no_channel).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

TEST(BitgetWs, OneChannelServesThreeSubscriptionKinds) {
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::Funding), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "trade");
    EXPECT_EQ(channel_for(exec::SubKind::Depth), "books15");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());

    EXPECT_EQ(inst_type(MarketType::Perp, false), "USDT-FUTURES");
    EXPECT_EQ(inst_type(MarketType::Perp, true), "SUSDT-FUTURES");
    // demo is futures-only, so spot has one instType
    EXPECT_EQ(inst_type(MarketType::Spot, false), "SPOT");
    EXPECT_EQ(inst_type(MarketType::Spot, true), "SPOT");
    // delivery lists under the usdt-m product
    EXPECT_EQ(inst_type(MarketType::Future, false), "USDT-FUTURES");
    // no crypto options; an empty instType is refused
    EXPECT_TRUE(inst_type(MarketType::Option, false).empty());
    EXPECT_EQ(stream_path(), "/v2/ws/public");
    EXPECT_EQ(private_stream_path(), "/v2/ws/private");
}

TEST(BitgetRest, EveryMixTargetCarriesProductType) {
    // a missing productType answers 400172 rather than defaulting
    RequestBuilder builder;
    EXPECT_EQ(builder.contracts(product_type(false)).target, "/api/v2/mix/market/contracts?productType=USDT-FUTURES");
    EXPECT_EQ(builder.ticker("BTCUSDT", product_type(false)).target,
              "/api/v2/mix/market/ticker?symbol=BTCUSDT&productType=USDT-FUTURES");
    EXPECT_EQ(builder.current_fund_rate("BTCUSDT", product_type(false)).target,
              "/api/v2/mix/market/current-fund-rate?symbol=BTCUSDT&productType=USDT-FUTURES");
    EXPECT_EQ(builder.orderbook("BTCUSDT", product_type(false), 5).target,
              "/api/v2/mix/market/orderbook?symbol=BTCUSDT&productType=USDT-FUTURES&limit=5");
    // the clock endpoint takes no productType
    EXPECT_EQ(builder.server_time().target, "/api/v2/public/time");
    EXPECT_EQ(product_type(true), "SUSDT-FUTURES");
}

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

TEST(BitgetVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue({.venue = VenueId::Bitget, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bitget);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(BitgetVenue, OrderEntryWithoutCredentialsRefusesLocally) {
    // bitget answers nothing for a request it never received, so the refusal is synthesized
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bitget, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);

    // the order a cancel targets is still alive
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);

    // no in-place amend, so the refusal is a verdict
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);
}

TEST(BitgetVenue, OptionsAndDeliveryFuturesRefuseByNameOnEveryPull) {
    // options and delivery must refuse; an empty vector would read as flat
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bitget, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    auto& execution = *adapter->execution;

    for (const auto market : {MarketType::Option, MarketType::Future}) {
        const auto orders = execution.open_order_reports(market);
        ASSERT_FALSE(orders.has_value());
        EXPECT_EQ(orders.error().cls, exec::ErrorClass::Terminal);
        EXPECT_FALSE(execution.fill_reports(market, UnixNanos{0}).has_value());
        EXPECT_FALSE(execution.position_reports(market).has_value());
        const auto id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
        EXPECT_FALSE(execution.query_order(id, market).has_value());
    }

    // spot holds coin balances, not positions
    const auto spot = execution.position_reports(MarketType::Spot);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().message.view(), "bitget spot position pull");
}

TEST(BitgetMap, SpotCatalogueRowsBecomeInstrumentsOrAreRefused) {
    // spot tick is 10^-pricePrecision
    auto storage = std::make_unique<SpotSymbolBuffer>();
    const auto body = fixture("rest_spot_symbols.json");
    const auto rows = parse_spot_symbols(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);

    exec::InstrumentTable table;
    std::size_t loaded = 0;
    for (const auto& row : *rows) {
        const auto instrument = map_instrument(row);
        if (instrument.has_value()) {
            ASSERT_TRUE(table.add(*instrument).has_value());
            ++loaded;
        }
    }
    // BABYDOGEUSDT's 13 decimals floor to a zero tick and REAUSDT is halted
    EXPECT_EQ(loaded, 3U);

    const auto* btc = base_for(table, "BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->tick_size.raw, 10'000'000); // 0.01
    EXPECT_EQ(btc->lot_size.raw, 1'000);       // 0.000001
    EXPECT_EQ(btc->id.market(), MarketType::Spot);
    EXPECT_EQ(btc->min_notional.raw, k_e9); // minTradeUSDT "1"

    // the finest tick e9 holds; a live row sits on it
    const auto* shib = base_for(table, "SHIBUSDT");
    ASSERT_NE(shib, nullptr);
    EXPECT_EQ(shib->tick_size.raw, 1);

    EXPECT_EQ(table.find(std::string_view{"BABYDOGEUSDT"}), nullptr);
    EXPECT_EQ(table.find(std::string_view{"REAUSDT"}), nullptr);
}

TEST(BitgetMap, ARwaRowIsNotACoin) {
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts_rwa.json");
    const auto rows = parse_contracts(body, *storage);
    ASSERT_TRUE(rows.has_value());
    std::map<std::string, std::string, std::less<>> keys;
    for (const auto& row : *rows) {
        auto instrument = map_instrument(row, MarketType::Perp);
        ASSERT_TRUE(instrument.has_value()) << row.symbol;
        classify(VenueId::Bitget, *instrument);
        keys.emplace(std::string{row.symbol}, asset_key(base_of(*instrument)));
    }
    EXPECT_EQ(keys["BTCUSDT"], "crypto:BTC");
    EXPECT_EQ(keys["AAPLUSDT"], "equity:AAPL");
    EXPECT_EQ(keys["IWMUSDT"], "equity:IWM");
    EXPECT_EQ(keys["XAUUSDT"], "commodity:XAU") << "the curated row names the metal";
    EXPECT_EQ(keys["PAXGUSDT"], "crypto:PAXG") << "rwa, but a token";
}
} // namespace
} // namespace raikiri::venues::bitget
