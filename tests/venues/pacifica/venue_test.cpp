#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/pacifica/endpoints.hpp>
#include <raikiri/venues/pacifica/map.hpp>
#include <raikiri/venues/pacifica/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::pacifica {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'412'400'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

WireMarket row(std::string_view symbol) {
    static const auto body = fixture("rest_info.json");
    static const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return one;
        }
    }
    ADD_FAILURE() << "no market " << symbol;
    return {};
}

InstrumentBase base_for(std::string_view symbol) {
    const auto instrument = map_instrument(row(symbol), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << symbol;
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Pacifica, MarketType::Perp, SymbolId{3});
    return base;
}

TEST(PacificaCatalogue, LoadsThePerpetualsAndLeavesTheSpotPairOut) {
    const auto body = fixture("rest_info.json");
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 6U);

    std::size_t perps = 0;
    for (const auto& one : *rows) {
        perps += map_instrument(one, MarketType::Perp).has_value() ? 1U : 0U;
        EXPECT_FALSE(map_instrument(one, MarketType::Spot).has_value());
    }
    EXPECT_EQ(perps, 5U);
    EXPECT_FALSE(map_instrument(row("SOL-USDC"), MarketType::Perp).has_value());

    const auto btc = base_for("BTC");
    EXPECT_EQ(btc.exchange_symbol.view(), "BTC");
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(btc.tick_size, e9("1"));
    EXPECT_EQ(btc.lot_size, e9("0.00001"));
    EXPECT_EQ(btc.min_notional, e9("10"));
    EXPECT_EQ(btc.kind, ContractKind::Linear);

    const auto instrument = map_instrument(row("BTC"), MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 3600U);

    EXPECT_EQ(base_for("kBONK").local_symbol.view(), "KBONK/USDC");
    EXPECT_TRUE(row("XAU").rfq);
    EXPECT_TRUE(row("CL").rfq);
    EXPECT_FALSE(row("BTC").rfq);
}

TEST(PacificaFrames, EveryCapturedFrameNamesItsChannel) {
    EXPECT_EQ(parse_channel(fixture("ws_book.json")), Channel::Book);
    EXPECT_EQ(parse_channel(fixture("ws_bbo.json")), Channel::Bbo);
    EXPECT_EQ(parse_channel(fixture("ws_trades.json")), Channel::Trades);
    EXPECT_EQ(parse_channel(fixture("ws_prices.json")), Channel::Prices);
    EXPECT_EQ(parse_channel(fixture("ws_pong.json")), Channel::Pong);
    EXPECT_EQ(parse_channel(fixture("ws_subscribe.json")), Channel::Subscribed);
    EXPECT_EQ(parse_channel(fixture("ws_error.json")), Channel::Error);
    EXPECT_EQ(parse_channel(R"({"channel":"candle","data":{}})"), Channel::Unknown);
    EXPECT_EQ(parse_channel("not json"), Channel::Unknown);
}

TEST(PacificaBook, EveryFrameIsTheWholeLadder) {
    const auto frame = fixture("ws_book.json");
    LevelBuffer storage;
    const auto wire = parse_book(frame, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC");
    EXPECT_EQ(wire->li, 13294587365ULL);
    ASSERT_EQ(wire->bids.size(), 10U);
    ASSERT_EQ(wire->asks.size(), 10U);

    const auto base = base_for("BTC");
    const auto book = map_book(*wire, base, k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 10U);
    EXPECT_EQ(book->ask_count, 10U);
    EXPECT_EQ(book->venue_seq, 13294587365ULL);
    EXPECT_EQ(book->bids[0].price, PriceTicks::from_raw(84266));
    EXPECT_EQ(book->bids[0].qty, QtyLots::from_raw(2649));
    EXPECT_EQ(book->asks[0].price, PriceTicks::from_raw(84267));
    EXPECT_EQ(book->asks[9].price, PriceTicks::from_raw(84276));
    EXPECT_EQ(book->ts_event, UnixNanos{1'790'412'391'815'000'000ULL});

    const auto top = top_of(*wire);
    ASSERT_TRUE(top.has_value());
    const auto bbo = map_bbo(*top, base, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid, PriceTicks::from_raw(84266));
    EXPECT_EQ(bbo->ask_qty, QtyLots::from_raw(177811));
}

TEST(PacificaBook, AnOffGridLevelDropsTheFrameRatherThanRoundingIt) {
    const std::string frame =
        R"({"channel":"book","data":{"s":"BTC","l":[[{"p":"84266.5","a":"1","n":1}],[{"p":"84267","a":"1","n":1}]],"t":1,"li":5}})";
    LevelBuffer storage;
    const auto wire = parse_book(frame, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_book(*wire, base_for("BTC"), k_ingest).has_value());
}

TEST(PacificaBook, LastIdSkipsGapsButRefusesARepeatOrAStepBack) {
    LastId gate;
    EXPECT_TRUE(gate.admit(13294527548ULL));
    EXPECT_TRUE(gate.admit(13294527599ULL));  // a gap: other symbols' events
    EXPECT_FALSE(gate.admit(13294527599ULL)); // the venue resends an unchanged book
    EXPECT_FALSE(gate.admit(13294527554ULL));
    EXPECT_TRUE(gate.admit(13294527600ULL));
    gate = {};
    EXPECT_TRUE(gate.admit(1));
}

TEST(PacificaBbo, MapsTheTopOfBook) {
    const auto frame = fixture("ws_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->li, 13294588035ULL);
    const auto bbo = map_bbo(*wire, base_for("BTC"), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid, PriceTicks::from_raw(84266));
    EXPECT_EQ(bbo->ask, PriceTicks::from_raw(84267));
    EXPECT_EQ(bbo->bid_qty, QtyLots::from_raw(19));
    EXPECT_EQ(bbo->ask_qty, QtyLots::from_raw(177811));
    EXPECT_EQ(bbo->ts_event, UnixNanos{1'790'412'393'563'000'000ULL});
}

TEST(PacificaTrades, TheDirectionNamesTheTakersSide) {
    const auto frame = fixture("ws_trades.json");
    PrintBuffer storage;
    const auto rows = parse_trades(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    EXPECT_EQ((*rows)[0].history_id, 285904184ULL);
    EXPECT_LT((*rows)[0].history_id, (*rows)[1].history_id);

    const auto base = base_for("BTC");
    const auto trade = map_trade((*rows)[0], base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price, PriceTicks::from_raw(84266));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(19));

    auto print = (*rows)[0];
    print.direction = "open_long";
    EXPECT_EQ(map_trade(print, base, k_ingest)->aggressor, Side::Buy);
    print.direction = "close_short";
    EXPECT_EQ(map_trade(print, base, k_ingest)->aggressor, Side::Buy);
    print.direction = "open_short";
    EXPECT_EQ(map_trade(print, base, k_ingest)->aggressor, Side::Sell);
    print.direction = "liquidate";
    EXPECT_FALSE(map_trade(print, base, k_ingest).has_value());
}

TEST(PacificaFunding, TheFixedRateTheEstimateAndTheNextHour) {
    const auto frame = fixture("ws_prices.json");
    const auto storage = std::make_unique<PriceBuffer>();
    const auto rows = parse_prices(frame, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 6U);

    const WirePrice* cl = nullptr;
    for (const auto& one : *rows) {
        if (one.symbol == "CL") {
            cl = &one;
        }
    }
    ASSERT_NE(cl, nullptr);
    const auto base = base_for("CL");
    const auto funding = map_funding(*cl, base, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, FixedE9::from_raw(-76'210));
    EXPECT_EQ(funding->predicted_rate, FixedE9::from_raw(-37'300));
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(94'061));
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->ts_event, UnixNanos{1'790'412'391'391'000'000ULL});
    EXPECT_EQ(funding->next_funding_time, UnixNanos{1'790'413'200'000'000'000ULL});

    const auto stats = map_ticker_stats(*cl, base, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, FixedE6::from_raw(1'165'890'934'640));
    EXPECT_TRUE(stats->volume_24h_base.is_undef());

    WirePrice blank = *cl;
    blank.funding = "";
    EXPECT_FALSE(map_funding(blank, base, k_ingest).has_value());
    blank = *cl;
    blank.next_funding = "";
    blank.mark = "";
    const auto partial = map_funding(blank, base, k_ingest);
    ASSERT_TRUE(partial.has_value());
    EXPECT_TRUE(partial->predicted_rate.is_undef());
    EXPECT_TRUE(partial->mark_price.is_undef());
}

TEST(PacificaFunding, TheHourAfterASettlementPublishesTheRateNowAccruing) {
    // captured across 11:00 utc: seconds from the hour, funding, next_funding
    struct Row {
        double at;
        std::string_view funding;
        std::string_view next;
        std::int64_t rate;
        std::int64_t predicted;
    };

    constexpr double k_hour = 1'790'420'400.0;
    constexpr auto k_undef = FixedE9::undef().raw;
    const auto run = [&](std::initializer_list<Row> rows) {
        FundingRoll roll;
        const auto base = base_for("CL");
        for (const auto& row : rows) {
            const WirePrice wire{.symbol = "CL",
                                 .funding = row.funding,
                                 .next_funding = row.next,
                                 .mark = "94.06",
                                 .volume_24h = "1",
                                 .ts_ms = static_cast<std::uint64_t>((k_hour + row.at) * 1000.0)};
            auto update = map_funding(wire, base, k_ingest);
            ASSERT_TRUE(update.has_value());
            roll.apply(*update);
            EXPECT_EQ(update->rate.raw, row.rate) << row.at;
            EXPECT_EQ(update->predicted_rate.raw, row.predicted) << row.at;
        }
    };
    // SOL: next_funding settles at +4 s, funding takes it at +19 s.
    run({{-1.6, "0.00000283", "0.00001119", 2'830, 11'190},
         {4.4, "0.00000283", "0.00001121", 11'210, k_undef},
         {19.4, "0.00001121", "0.0000058", 11'210, 5'800},
         {25.4, "0.00001121", "0.00000696", 11'210, 6'960}});
    // CL: only funding moves.
    run({{-4.6, "-0.00000499", "0.0000125", -4'990, 12'500},
         {4.4, "-0.00000499", "0.0000125", 12'500, k_undef},
         {25.4, "0.0000125", "0.0000125", 12'500, 12'500}});
    // ENA: the rate paid and the rate accruing are equal, so the roll is next_funding moving.
    run({{-4.6, "0.0000125", "0.0000125", 12'500, 12'500},
         {4.4, "0.0000125", "0.0000125", 12'500, k_undef},
         {25.4, "0.0000125", "-0.00009899", 12'500, -98'990}});
    // BTC: nothing moves across the hour, so the hold lapses.
    run({{-4.6, "0.0000125", "0.0000125", 12'500, 12'500},
         {60.0, "0.0000125", "0.0000125", 12'500, k_undef},
         {90.0, "0.0000125", "0.0000125", 12'500, 12'500}});
}

TEST(PacificaFunding, SettlementIsTheNextTopOfTheHour) {
    EXPECT_EQ(next_settlement(UnixNanos{3'600'000'000'000ULL}), UnixNanos{7'200'000'000'000ULL});
    EXPECT_EQ(next_settlement(UnixNanos{3'599'999'999'999ULL}), UnixNanos{3'600'000'000'000ULL});
}

TEST(PacificaCaps, PerpMarketDataOnly) {
    const auto& caps = exec::caps_of(VenueId::Pacifica);
    EXPECT_EQ(caps.venue, VenueId::Pacifica);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_EQ(caps.max_client_order_id_len, 0U);
    EXPECT_TRUE(caps.ticker_stats);
    EXPECT_EQ(to_string(VenueId::Pacifica), "pacifica");
    EXPECT_EQ(venue_from_string("pacifica"), VenueId::Pacifica);
    EXPECT_EQ(static_cast<int>(VenueId::Pacifica), 30);
}

TEST(PacificaMap, TheCuratedTableClassesEquitiesAndMetals) {
    const auto classified = [](WireMarket wire) {
        auto instrument = map_instrument(wire, MarketType::Perp);
        EXPECT_TRUE(instrument.has_value()) << wire.symbol;
        if (!instrument.has_value()) {
            return InstrumentBase{};
        }
        classify(VenueId::Pacifica, *instrument);
        return base_of(*instrument);
    };
    const auto xau = classified(row("XAU"));
    EXPECT_EQ(xau.asset_class, AssetClass::Commodity);
    EXPECT_EQ(xau.underlying.view(), "XAU");
    const auto cl = classified(row("CL"));
    EXPECT_EQ(cl.asset_class, AssetClass::Commodity);
    EXPECT_EQ(cl.underlying.view(), "CL");
    const auto btc = classified(row("BTC"));
    EXPECT_EQ(btc.asset_class, AssetClass::Crypto);
    EXPECT_EQ(btc.underlying.view(), "BTC");

    auto tsla = row("BTC");
    tsla.symbol = "TSLA";
    const auto equity = classified(tsla);
    EXPECT_EQ(equity.asset_class, AssetClass::Equity);
    EXPECT_EQ(equity.underlying.view(), "TSLA");
}

TEST(PacificaMap, TheLiveListingsEquitiesAreNotReadAsCoins) {
    const auto body = fixture("rest_info_live.json");
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    const auto cls = [&](std::string_view symbol) {
        for (const auto& one : *rows) {
            if (one.symbol != symbol) {
                continue;
            }
            auto instrument = map_instrument(one, MarketType::Perp);
            if (!instrument.has_value()) {
                return AssetClass::Unknown;
            }
            classify(VenueId::Pacifica, *instrument);
            return base_of(*instrument).asset_class;
        }
        ADD_FAILURE() << symbol << " not listed";
        return AssetClass::Unknown;
    };
    for (const auto* equity : {"SAMSUNG", "SKHYNIX", "SPCX", "URNM", "DRAM"}) {
        EXPECT_EQ(cls(equity), AssetClass::Equity) << equity;
    }
    // BP is backpack's token
    EXPECT_EQ(cls("BP"), AssetClass::Crypto);
}

} // namespace
} // namespace raikiri::venues::pacifica
