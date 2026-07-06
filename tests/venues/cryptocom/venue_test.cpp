#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/cryptocom/book.hpp>
#include <raikiri/venues/cryptocom/catalogue.hpp>
#include <raikiri/venues/cryptocom/endpoints.hpp>
#include <raikiri/venues/cryptocom/envelope.hpp>
#include <raikiri/venues/cryptocom/map.hpp>
#include <raikiri/venues/cryptocom/market_data.hpp>
#include <raikiri/venues/cryptocom/ws.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::cryptocom {
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

// Heap-held so the event's views outlive the return.
struct Framed {
    std::unique_ptr<std::string> frame;
    Event event;
};

Framed framed(std::string_view name) {
    Framed out{std::make_unique<std::string>(fixture(name)), {}};
    const auto event = parse_event(*out.frame);
    EXPECT_TRUE(event.has_value()) << name;
    if (event.has_value()) {
        out.event = *event;
    }
    return out;
}

const WireInstrument* row_named(std::span<const WireInstrument> rows, std::string_view symbol) {
    for (const auto& row : rows) {
        if (row.symbol == symbol) {
            return &row;
        }
    }
    return nullptr;
}

struct Catalogue {
    std::unique_ptr<InstrumentBuffer> storage = std::make_unique<InstrumentBuffer>();
    std::string body = fixture("rest_instruments.json");
    std::span<const WireInstrument> rows;

    Catalogue() {
        const auto parsed = parse_instruments(body, *storage);
        EXPECT_TRUE(parsed.has_value());
        if (parsed.has_value()) {
            rows = *parsed;
        }
    }
};

exec::InstrumentTable table_for(MarketType market) {
    Catalogue catalogue;
    exec::InstrumentTable table;
    for (const auto& row : catalogue.rows) {
        if (const auto instrument = map_instrument(row, market); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

const InstrumentBase& listed(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    return base_of(*instrument);
}

TEST(CryptoComCatalogue, OneAnswerCarriesEveryInstrumentType) {
    Catalogue catalogue;
    ASSERT_EQ(catalogue.rows.size(), 7U);
    const auto* btc = row_named(catalogue.rows, "BTCUSD-PERP");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->inst_type, "PERPETUAL_SWAP");
    EXPECT_EQ(btc->base_ccy, "BTC");
    EXPECT_EQ(btc->quote_ccy, "USD");
    EXPECT_EQ(btc->price_tick_size, "0.1");
    EXPECT_EQ(btc->qty_tick_size, "0.0001");
    EXPECT_EQ(btc->contract_size, "1");
    EXPECT_TRUE(btc->tradable);
    const auto* pair = row_named(catalogue.rows, "BTC_USD");
    ASSERT_NE(pair, nullptr);
    EXPECT_EQ(pair->inst_type, "CCY_PAIR");
    EXPECT_TRUE(pair->contract_size.empty());
}

TEST(CryptoComCatalogue, EachMarketTakesOnlyItsOwnRows) {
    const auto spot = table_for(MarketType::Spot);
    const auto perp = table_for(MarketType::Perp);
    EXPECT_EQ(spot.size(), 2U);
    EXPECT_EQ(perp.size(), 2U);
    EXPECT_NE(spot.find("BTC_USD"), nullptr);
    EXPECT_NE(spot.find("BTC_USDT"), nullptr);
    EXPECT_NE(perp.find("BTCUSD-PERP"), nullptr);
    EXPECT_NE(perp.find("AAPLUSD-PERP"), nullptr);
    EXPECT_EQ(spot.find("BTCUSD-260925"), nullptr);
    EXPECT_EQ(perp.find("BTCUSD-260925"), nullptr);
    EXPECT_EQ(spot.find("CRO_BTC"), nullptr);
    EXPECT_EQ(perp.find("MOGUSD-PERP"), nullptr);
}

TEST(CryptoComCatalogue, APerpetualCarriesTheMeasuredHourlyInterval) {
    const auto perp = table_for(MarketType::Perp);
    const auto* btc = perp.find("BTCUSD-PERP");
    ASSERT_NE(btc, nullptr);
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*btc));
    const auto& contract = std::get<PerpInstrument>(*btc);
    EXPECT_EQ(contract.funding_interval_s, 3600U);
    EXPECT_TRUE(contract.funding_cap.is_undef());
    EXPECT_TRUE(contract.funding_floor.is_undef());
    const auto& base = contract.base;
    EXPECT_EQ(base.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(base.kind, ContractKind::Linear);
    EXPECT_EQ(base.contract_value.raw, k_e9);
    EXPECT_EQ(base.tick_size.raw, 100'000'000);
    EXPECT_EQ(base.lot_size.raw, 100'000);
    EXPECT_TRUE(base.min_notional.is_undef());
    EXPECT_EQ(base.id.venue(), VenueId::CryptoCom);
    EXPECT_EQ(base.id.market(), MarketType::Perp);
}

TEST(CryptoComCatalogue, APairKeepsItsQuoteAndLoadsAsLinear) {
    const auto spot = table_for(MarketType::Spot);
    const auto& usd = listed(spot, "BTC_USD");
    const auto& usdt = listed(spot, "BTC_USDT");
    EXPECT_EQ(usd.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(usdt.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(usd.kind, ContractKind::Linear);
    EXPECT_EQ(usd.contract_value.raw, k_e9);
    EXPECT_TRUE(validate(usd));
    EXPECT_TRUE(std::holds_alternative<SpotInstrument>(*spot.find("BTC_USD")));
}

TEST(CryptoComCatalogue, TheProductTypeIsTheAssetClass) {
    Catalogue catalogue;
    const auto classified = [](const WireInstrument& row) {
        const auto instrument = map_instrument(row, MarketType::Perp);
        EXPECT_TRUE(instrument.has_value()) << row.symbol;
        auto base = base_of(instrument.value_or(PerpInstrument{}));
        const auto stated = base.asset_class;
        classify(VenueId::CryptoCom, base);
        return std::pair{stated, base};
    };
    const auto [aapl_stated, aapl] = classified(*row_named(catalogue.rows, "AAPLUSD-PERP"));
    EXPECT_EQ(aapl_stated, AssetClass::Equity);
    EXPECT_EQ(aapl.underlying.view(), "AAPL");

    auto xau_row = *row_named(catalogue.rows, "BTCUSD-PERP");
    xau_row.symbol = "XAUUSD-PERP";
    xau_row.base_ccy = "XAU";
    xau_row.product_type = "COMMODITIES";
    const auto [xau_stated, xau] = classified(xau_row);
    EXPECT_EQ(xau_stated, AssetClass::Commodity);
    EXPECT_EQ(xau.underlying.view(), "XAU");

    auto spx_row = *row_named(catalogue.rows, "BTCUSD-PERP");
    spx_row.symbol = "SPXUSD-PERP";
    spx_row.base_ccy = "SPX";
    const auto [spx_stated, spx] = classified(spx_row);
    EXPECT_EQ(spx_stated, AssetClass::Crypto);
    EXPECT_EQ(spx.asset_class, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");

    spx_row.product_type = "BONDS";
    const auto [odd_stated, odd] = classified(spx_row);
    EXPECT_EQ(odd_stated, AssetClass::Unknown);
    EXPECT_EQ(odd.asset_class, AssetClass::Unknown);
}

TEST(CryptoComCatalogue, ANonTradableRowIsDropped) {
    Catalogue catalogue;
    auto row = *row_named(catalogue.rows, "BTCUSD-PERP");
    row.tradable = false;
    EXPECT_FALSE(map_instrument(row, MarketType::Perp).has_value());
    row.tradable = true;
    row.contract_size = {};
    EXPECT_FALSE(map_instrument(row, MarketType::Perp).has_value());
}

TEST(CryptoComEnvelope, RestStatusReadsSuccessAndRefusal) {
    const auto ok = parse_rest_status(fixture("rest_instruments.json"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, k_ok);
    const auto body = fixture("rest_error.json");
    const auto refused = parse_rest_status(body);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->code, k_missing_argument);
    EXPECT_EQ(refused->message, "Invalid instrument_name");
    EXPECT_EQ(classify(refused->code), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
}

TEST(CryptoComEnvelope, HeartbeatIsRecognisedAndAnsweredWithItsOwnId) {
    const auto hb = framed("ws_heartbeat.json");
    EXPECT_EQ(hb.event.kind, EventKind::Heartbeat);
    EXPECT_EQ(hb.event.id, 1790281754596);
    Protocol protocol;
    EXPECT_EQ(protocol.heartbeat_reply(hb.event.id),
              R"({"id":1790281754596,"method":"public/respond-heartbeat"})");
}

TEST(CryptoComEnvelope, AcksAndRefusalsAreToldApart) {
    const auto ack = framed("ws_book_ack.json");
    EXPECT_EQ(ack.event.kind, EventKind::Ack);
    EXPECT_EQ(ack.event.channel, "book.BTC_USD.50");

    const auto full = framed("ws_error_max_subscriptions.json");
    EXPECT_EQ(full.event.kind, EventKind::Error);
    EXPECT_EQ(full.event.code, k_max_subscriptions);
    EXPECT_EQ(full.event.id, 42);
    EXPECT_TRUE(full.event.channel.empty());

    const auto combo = framed("ws_error_unsupported.json");
    EXPECT_EQ(combo.event.kind, EventKind::Error);
    EXPECT_EQ(combo.event.code, k_invalid_request);
    EXPECT_EQ(combo.event.channel, "book.BTCUSD-PERP.50");
    EXPECT_EQ(combo.event.message, "Unsupported depth/interval combination");
}

TEST(CryptoComEnvelope, EveryDataChannelResolvesAndAnswersAreNotPushes) {
    const std::vector<std::pair<std::string_view, EventKind>> cases{
        {"ws_book_perp.json", EventKind::Book},
        {"ws_book_update_perp.json", EventKind::BookUpdate},
        {"ws_trade_perp.json", EventKind::Trade},
        {"ws_ticker_spot.json", EventKind::Ticker},
        {"ws_funding_perp_initial.json", EventKind::Funding},
        {"ws_estimatedfunding_perp_initial.json", EventKind::EstimatedFunding},
        {"ws_mark_perp.json", EventKind::Mark},
    };
    for (const auto& [name, kind] : cases) {
        const auto one = framed(name);
        EXPECT_EQ(one.event.kind, kind) << name;
        EXPECT_FALSE(one.event.data.empty()) << name;
        EXPECT_FALSE(one.event.instrument.empty()) << name;
    }
    EXPECT_TRUE(framed("ws_trade_perp.json").event.is_push());
    EXPECT_FALSE(framed("ws_trade_perp_initial.json").event.is_push());
    EXPECT_EQ(framed("ws_trade_perp_initial.json").event.id, 7);
    EXPECT_EQ(framed("ws_ticker_spot.json").event.instrument, "BTC_USD");
}

TEST(CryptoComBook, SnapshotSeedsAndTheCapturedDeltasChain) {
    const auto perp = table_for(MarketType::Perp);
    const auto& base = listed(perp, "BTCUSD-PERP");
    Book book{base.id, base.tick_size, base.lot_size};
    LevelBuffer levels;

    const auto snap_frame = framed("ws_book_perp.json");
    const auto snap = parse_book(snap_frame.event.data, true, levels);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bids.size(), 50U);
    EXPECT_EQ(snap->asks.size(), 50U);
    EXPECT_EQ(snap->asks.front().price, "84232.2");
    EXPECT_EQ(snap->asks.front().qty, "0.0051");
    EXPECT_EQ(book.apply(*snap), BookResult::Resynced);

    const auto bbo = book.bbo(ms_to_unix(snap->ts_ms, k_ingest), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->ask.raw, 842'322); // 84232.2 at tick 0.1
    EXPECT_EQ(bbo->ask_qty.raw, 51);  // 0.0051 at lot 0.0001
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);

    EXPECT_EQ(snap->seq, 370361387334144U);
    const auto first_frame = framed("ws_book_update_perp.json");
    const auto first = parse_book(first_frame.event.data, false, levels);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->prev_seq, snap->seq);
    EXPECT_EQ(book.apply(*first), BookResult::Applied);

    const auto next_frame = framed("ws_book_update_perp_next.json");
    const auto next = parse_book(next_frame.event.data, false, levels);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->prev_seq, first->seq);
    EXPECT_EQ(book.apply(*next), BookResult::Applied);
    EXPECT_TRUE(book.synced());
    EXPECT_EQ(book.gaps(), 0U);
    const auto after = book.bbo(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->ask.raw, 842'322);
    EXPECT_EQ(after->ask_qty.raw, 51);
    const auto depth = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->venue_seq, next->seq);
}

TEST(CryptoComBook, ABrokenChainDropsTheBookUntilTheNextSnapshot) {
    const auto perp = table_for(MarketType::Perp);
    const auto& base = listed(perp, "BTCUSD-PERP");
    Book book{base.id, base.tick_size, base.lot_size};
    LevelBuffer snap_levels;
    LevelBuffer delta_levels;
    const auto snap_frame = framed("ws_book_perp.json");
    const auto snap = parse_book(snap_frame.event.data, true, snap_levels);
    const auto next_frame = framed("ws_book_update_perp_next.json");
    const auto next = parse_book(next_frame.event.data, false, delta_levels);
    ASSERT_TRUE(snap.has_value());
    ASSERT_TRUE(next.has_value());

    EXPECT_EQ(book.apply(*next), BookResult::Ignored);
    ASSERT_EQ(book.apply(*snap), BookResult::Resynced);
    EXPECT_EQ(book.apply(*next), BookResult::Gap);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_FALSE(book.bbo(k_ingest, k_ingest).has_value());
    EXPECT_EQ(book.apply(*snap), BookResult::Resynced);
    EXPECT_TRUE(book.snapshot(k_ingest, k_ingest).has_value());
}

// Both aborted inside simdjson before fields were walked in order.
TEST(CryptoComBook, MalformedFramesAreRefusedNotTrusted) {
    LevelBuffer levels;
    for (const auto name : {"fuzz_book_trailing_comma.json", "fuzz_book_update_unclosed.json"}) {
        const auto body = fixture(name);
        EXPECT_FALSE(parse_book(body, true, levels).has_value()) << name;
        EXPECT_FALSE(parse_book(body, false, levels).has_value()) << name;
        EXPECT_FALSE(parse_event(body).has_value()) << name;
    }
}

TEST(CryptoComBook, AZeroSizeDeletesTheLevel) {
    const auto perp = table_for(MarketType::Perp);
    const auto& base = listed(perp, "BTCUSD-PERP");
    Book book{base.id, base.tick_size, base.lot_size};
    const std::array<WireLevel, 2> bids{{{"100.0", "1.0000"}, {"99.9", "2.0000"}}};
    const std::array<WireLevel, 1> asks{{{"100.1", "1.0000"}}};
    ASSERT_EQ(book.apply({.snapshot = true, .seq = 10, .bids = bids, .asks = asks}),
              BookResult::Resynced);
    const std::array<WireLevel, 1> removal{{{"100.0", "0"}}};
    ASSERT_EQ(book.apply({.snapshot = false, .seq = 11, .prev_seq = 10, .bids = removal}),
              BookResult::Applied);
    const auto bbo = book.bbo(k_ingest, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 999);
}

TEST(CryptoComBook, AnOffStepLevelInvalidatesRatherThanRounds) {
    const auto perp = table_for(MarketType::Perp);
    const auto& base = listed(perp, "BTCUSD-PERP");
    Book book{base.id, base.tick_size, base.lot_size};
    const std::array<WireLevel, 1> bids{{{"100.05", "1.0000"}}};
    const std::array<WireLevel, 1> asks{{{"100.1", "1.0000"}}};
    EXPECT_EQ(book.apply({.snapshot = true, .seq = 10, .bids = bids, .asks = asks}),
              BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
}

TEST(CryptoComTrades, APushMapsItsTakerSide) {
    const auto perp = table_for(MarketType::Perp);
    const auto& base = listed(perp, "BTCUSD-PERP");
    const auto one = framed("ws_trade_perp.json");
    TradeBuffer storage;
    const auto rows = parse_trades(one.event.data, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->front().trade_id, "1790281771941706234");
    const auto trade = map_trade(rows->front(), base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 842'322);
    EXPECT_EQ(trade->qty.raw, 1);
    EXPECT_EQ(trade->ts_event.ns, 1790281771941ULL * 1'000'000ULL);
}

TEST(CryptoComTrades, TheReplayParsesWhole) {
    const auto spot = table_for(MarketType::Spot);
    const auto& base = listed(spot, "BTC_USD");
    const auto one = framed("ws_trade_spot_initial.json");
    TradeBuffer storage;
    const auto rows = parse_trades(one.event.data, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 50U);
    const auto trade = map_trade(rows->front(), base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
}

TEST(CryptoComTrades, AnUnknownSideIsNotGuessed) {
    const auto perp = table_for(MarketType::Perp);
    const auto& base = listed(perp, "BTCUSD-PERP");
    WireTrade wire{.trade_id = "1", .price = "100.0", .qty = "1.0000", .side = "", .trade_ms = 1};
    EXPECT_FALSE(map_trade(wire, base, k_ingest).has_value());
}

TEST(CryptoComTicker, BothTurnoverColumnsAreRead) {
    const auto perp = table_for(MarketType::Perp);
    const auto one = framed("ws_ticker_perp.json");
    const auto wire = parse_ticker(one.event.data);
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_ticker_stats(*wire, listed(perp, "BTCUSD-PERP"), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 4'187'826'600'000);        // 4187.8266 BTC
    EXPECT_EQ(stats->volume_24h_quote.raw, 352'032'445'730'000); // 352032445.73 USD
}

TEST(CryptoComTicker, AnAbsentColumnIsUndefNotZero) {
    const auto spot = table_for(MarketType::Spot);
    const WireTicker quote_only{.volume = {}, .quote_volume = "12.5", .ts_ms = 1};
    const auto stats = map_ticker_stats(quote_only, listed(spot, "BTC_USD"), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_TRUE(stats->volume_24h_base.is_undef());
    const WireTicker neither{};
    EXPECT_FALSE(map_ticker_stats(neither, listed(spot, "BTC_USD"), k_ingest).has_value());
}

TEST(CryptoComFunding, SettlementIsTheNextTopOfTheHour) {
    EXPECT_EQ(next_settlement_ms(1790281745000ULL), 1790283600000ULL);
    EXPECT_EQ(next_settlement_ms(1790280000000ULL), 1790283600000ULL);
    EXPECT_EQ(next_settlement_ms(0), 0U);
}

TEST(CryptoComFunding, ThreeChannelsFoldIntoOneRecord) {
    const auto perp = table_for(MarketType::Perp);
    const auto& base = listed(perp, "BTCUSD-PERP");
    FundingMemo memo;

    const auto mark_frame = framed("ws_mark_perp.json");
    const auto mark = parse_value(mark_frame.event.data);
    ASSERT_TRUE(mark.has_value());
    EXPECT_FALSE(map_mark(*mark, base, memo, k_ingest).has_value());

    const auto rate_frame = framed("ws_funding_perp_initial.json");
    const auto rate = parse_value(rate_frame.event.data);
    ASSERT_TRUE(rate.has_value());
    EXPECT_EQ(rate->value, "-0.000005641");
    const auto first = map_funding_rate(*rate, base, memo, k_ingest);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->rate.raw, -5'641);
    EXPECT_EQ(first->interval_s, 3600U);
    EXPECT_EQ(first->next_funding_time.ns, 1790283600000ULL * 1'000'000ULL);
    EXPECT_EQ(first->mark_price.raw, 842'369);
    EXPECT_TRUE(first->predicted_rate.is_undef());

    const auto est_frame = framed("ws_estimatedfunding_perp_initial.json");
    const auto estimated = parse_value(est_frame.event.data);
    ASSERT_TRUE(estimated.has_value());
    const auto second = map_estimated_rate(*estimated, base, memo, k_ingest);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->predicted_rate.raw, -2'445);
    EXPECT_EQ(second->rate.raw, -5'641);
    EXPECT_EQ(second->instrument, base.id);
}

TEST(CryptoComFunding, AnOffTickMarkIsRefused) {
    const auto perp = table_for(MarketType::Perp);
    FundingMemo memo;
    memo.rate = FixedE9::from_raw(1);
    const WireValue off{.value = "84236.95", .ts_ms = 1};
    EXPECT_FALSE(map_mark(off, listed(perp, "BTCUSD-PERP"), memo, k_ingest).has_value());
    EXPECT_TRUE(memo.mark.is_undef());
}

TEST(CryptoComProtocol, FramesStayUnderTheSizeTheVenueSilentlyDrops) {
    std::vector<std::string> names;
    for (int i = 0; i < 400; ++i) {
        names.push_back("book.SYMBOL" + std::to_string(i) + "USD-PERP.50");
    }
    const std::vector<std::string_view> channels{names.begin(), names.end()};
    Protocol protocol;
    std::size_t built = 0;
    std::size_t frames = 0;
    while (built < channels.size()) {
        const auto frame =
            protocol.subscribe_frame(std::span<const std::string_view>{channels}.subspan(built));
        ASSERT_TRUE(frame.has_value());
        ASSERT_GT(frame->consumed, 0U);
        EXPECT_LE(frame->text.size(), ep::k_max_frame_bytes);
        EXPECT_NE(frame->text.find(R"("book_subscription_type":"SNAPSHOT_AND_UPDATE")"),
                  std::string_view::npos);
        EXPECT_TRUE(frame->text.ends_with(R"("book_update_frequency":100}})"));
        built += frame->consumed;
        ++frames;
    }
    EXPECT_EQ(built, 400U);
    EXPECT_GE(frames, 3U);
}

TEST(CryptoComProtocol, OneFrameReadsAsTheVenueExpects) {
    Protocol protocol;
    const std::array<std::string_view, 2> channels{"book.BTC_USD.50", "trade.BTC_USD"};
    const auto frame = protocol.subscribe_frame(channels);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(
        frame->text,
        R"({"id":1,"method":"subscribe","params":{"channels":["book.BTC_USD.50","trade.BTC_USD"],)"
        R"("book_subscription_type":"SNAPSHOT_AND_UPDATE","book_update_frequency":100}})");
}

TEST(CryptoComProtocol, ChannelNamesRoundTrip) {
    std::array<char, k_max_channel_len> buf{};
    const auto book = book_channel("BTCUSD-PERP", buf);
    EXPECT_EQ(book, "book.BTCUSD-PERP.50");
    EXPECT_EQ(instrument_of(book), "BTCUSD-PERP");
    EXPECT_EQ(instrument_of("estimatedfunding.BTCUSD-PERP"), "BTCUSD-PERP");
    EXPECT_EQ(instrument_of("trade.BTC_USD"), "BTC_USD");
    std::array<char, 8> tiny{};
    EXPECT_TRUE(channel_name(k_trade_prefix, "BTCUSD-PERP", tiny).empty());
}

TEST(CryptoComCaps, TheRowServesBothMarketsAndTradesNothing) {
    const auto& caps = exec::caps_of(VenueId::CryptoCom);
    EXPECT_EQ(caps.venue, VenueId::CryptoCom);
    EXPECT_TRUE(caps.markets.spot);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.future);
    EXPECT_FALSE(caps.cancel_by_client_order_id);
    EXPECT_FALSE(caps.position_pull.any());
    EXPECT_FALSE(caps.funding_charge.any());
    EXPECT_TRUE(caps.ticker_stats);
    EXPECT_EQ(to_string(VenueId::CryptoCom), "cryptocom");
    EXPECT_EQ(venue_from_string("cryptocom"), VenueId::CryptoCom);
}

} // namespace
} // namespace raikiri::venues::cryptocom
