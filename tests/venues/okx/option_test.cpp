#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/key.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::okx {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

FixedE9 e9(std::string_view text) {
    return *parse_decimal_e9(text);
}

// the catalogue as the source loads it: coin-margined first, classified, then tabled
struct Loaded {
    std::string body;
    exec::InstrumentTable table;
    std::vector<Instrument> catalogue;
};

std::unique_ptr<Loaded> load_catalogue() {
    auto out = std::make_unique<Loaded>();
    out->body = fixture("rest_option_instruments.json");
    static InstrumentBuffer storage;
    const auto rows = parse_instruments(out->body, storage);
    EXPECT_TRUE(rows.has_value());
    if (!rows.has_value()) {
        return out;
    }
    std::vector<Instrument> mapped;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Option); instrument.has_value()) {
            classify(VenueId::Okx, *instrument);
            mapped.push_back(*instrument);
        }
    }
    std::ranges::stable_sort(mapped, {}, [](const Instrument& instrument) {
        return std::get<OptionInstrument>(instrument).base.kind == ContractKind::Inverse ? 0 : 1;
    });
    for (const auto& instrument : mapped) {
        if (const auto id = out->table.add(instrument); id.has_value()) {
            out->catalogue.push_back(*out->table.find(*id));
        }
    }
    return out;
}

const OptionInstrument& option_named(const Loaded& loaded, std::string_view name) {
    const auto* found = loaded.table.find(name);
    EXPECT_NE(found, nullptr) << name;
    return std::get<OptionInstrument>(*found);
}

TEST(OkxOptionCatalogue, ListsEveryUnderlyingThatTradesOptions) {
    const auto body = fixture("rest_option_underlyings.json");
    UnderlyingBuffer storage;
    const auto names = parse_underlyings(body, storage);
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names->size(), 4U);
    EXPECT_NE(std::ranges::find(*names, "BTC-USD"), names->end());
    EXPECT_NE(std::ranges::find(*names, "ETH-USD"), names->end());
}

TEST(OkxOptionCatalogue, MapsCoinMarginedAndUsdMarginedFamilies) {
    const auto loaded = load_catalogue();
    // the three BTC-USD_UM twins are 25 bytes long, one past SymbolText
    ASSERT_EQ(loaded->catalogue.size(), 5U);
    EXPECT_EQ(loaded->table.find("BTC-USD_UM-261009-75000-P"), nullptr);

    const auto& inverse = option_named(*loaded, "BTC-USD-261009-75000-P");
    EXPECT_EQ(inverse.put_call, PutCall::Put);
    EXPECT_EQ(inverse.base.kind, ContractKind::Inverse);
    EXPECT_EQ(inverse.premium, OptionUnit::Base);
    EXPECT_EQ(inverse.greeks, OptionUnit::Quote);
    EXPECT_EQ(inverse.base.tick_size, e9("0.0001"));
    EXPECT_EQ(inverse.base.contract_value, e9("0.01"));
    EXPECT_EQ(inverse.base.underlying.view(), "BTC");
    EXPECT_EQ(inverse.base.ticks_to_price(inverse.strike), e9("75000"));
    const auto key = options::key_of(inverse);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "BTC-20261009-75000-P");
    EXPECT_EQ(inverse.expiry.ns % 86'400'000'000'000ULL, 8ULL * 3'600'000'000'000ULL);

    const auto& linear = option_named(*loaded, "SOL-USD_UM-261004-95-C");
    EXPECT_EQ(linear.put_call, PutCall::Call);
    EXPECT_EQ(linear.base.kind, ContractKind::Linear);
    EXPECT_EQ(linear.premium, OptionUnit::Quote);
    EXPECT_EQ(linear.greeks, OptionUnit::Quote);
    EXPECT_EQ(linear.base.contract_value, e9("0.1"));
    EXPECT_EQ(linear.base.underlying.view(), "SOL");
    const auto sol = options::key_of(linear);
    ASSERT_TRUE(sol.has_value());
    EXPECT_EQ(options::canonical_name(*sol), "SOL-20261004-95-C");
}

TEST(OkxOptionCatalogue, RefusesARowWhoseStrikeIsOffTheTickOrThatIsNotAnOption) {
    WireInstrument row{};
    row.inst_id = "BTC-USD-261009-75000-P";
    row.inst_type = "OPTION";
    row.state = "live";
    row.tick_size = "0.0001";
    row.lot_size = "1";
    row.ct_val = "1";
    row.ct_mult = "0.01";
    row.ct_type = "inverse";
    row.strike = "75000";
    row.opt_type = "P";
    row.expiry_ms = 1'791'446'400'000ULL;
    EXPECT_TRUE(map_instrument(row, MarketType::Option).has_value());
    EXPECT_FALSE(map_instrument(row, MarketType::Perp).has_value());

    auto off_tick = row;
    off_tick.strike = "75000.00005";
    EXPECT_FALSE(map_instrument(off_tick, MarketType::Option).has_value());
    auto no_side = row;
    no_side.opt_type = "";
    EXPECT_FALSE(map_instrument(no_side, MarketType::Option).has_value());
    auto swap = row;
    swap.inst_type = "SWAP";
    EXPECT_FALSE(map_instrument(swap, MarketType::Option).has_value());
}

TEST(OkxOptionChain, GroupsTheCatalogueByUnderlyingExpiryAndStrike) {
    const auto loaded = load_catalogue();
    const auto chain = options::OptionChain::build(loaded->catalogue);
    EXPECT_EQ(chain.size(), 5U);
    const auto* btc = chain.underlying("BTC");
    ASSERT_NE(btc, nullptr);
    ASSERT_EQ(btc->expiries.size(), 2U);
    EXPECT_LT(btc->expiries[0].expiry, btc->expiries[1].expiry);
    ASSERT_EQ(btc->expiries[0].rows.size(), 2U);
    EXPECT_EQ(btc->expiries[0].rows[0].strike, e9("80000"));
    EXPECT_EQ(btc->expiries[0].rows[1].strike, e9("84900"));
    EXPECT_FALSE(btc->expiries[0].rows[0].put.has_value());
    ASSERT_EQ(btc->expiries[1].rows.size(), 2U);
    EXPECT_TRUE(btc->expiries[1].rows[0].put.has_value());
    ASSERT_NE(chain.underlying("SOL"), nullptr);

    const auto key = options::key_of(option_named(*loaded, "BTC-USD-261009-75000-P"));
    ASSERT_TRUE(key.has_value());
    const auto found = chain.find(*key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(loaded->table.exchange_symbol(*found), "BTC-USD-261009-75000-P");
}

TEST(OkxOptionGreeks, TakesTheUsdBlackScholesFiguresAndTheMemoisedMarkAndIndex) {
    const auto loaded = load_catalogue();
    const auto frame = fixture("ws_opt_summary.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::OptSummary);

    static SummaryBuffer rows;
    const auto wire = parse_opt_summary(frame, rows);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 3U);
    EXPECT_EQ((*wire)[0].inst_id, "BTC-USD-261009-75000-P");

    const OptionMemo memo{.mark = e9("0.0001"), .index = e9("84910.1")};
    const auto greeks = map_option_greeks((*wire)[0], loaded->table, memo, UnixNanos{1});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument, base_of(*loaded->table.find("BTC-USD-261009-75000-P")).id);
    // deltaBS, not the premium-adjusted coin delta of -0.0162891307
    EXPECT_EQ(greeks->delta, e9("-0.015948624"));
    EXPECT_EQ(greeks->gamma, e9("0.000008018"));
    EXPECT_EQ(greeks->vega, e9("4.197636241"));
    EXPECT_EQ(greeks->theta, e9("-17.751723475"));
    EXPECT_EQ(greeks->mark_iv, e9("0.473392617"));
    EXPECT_EQ(greeks->bid_iv, e9("0.468759062"));
    EXPECT_EQ(greeks->ask_iv, e9("0.488290273"));
    EXPECT_EQ(greeks->underlying_price, e9("84907.865713335"));
    EXPECT_EQ(greeks->mark_price, e9("0.0001"));
    EXPECT_EQ(greeks->index_price, e9("84910.1"));
    EXPECT_EQ(greeks->ts_event.ns, 1'791'049'079'993'000'000ULL);

    const auto unmarked = map_option_greeks((*wire)[0], loaded->table, OptionMemo{}, UnixNanos{1});
    ASSERT_TRUE(unmarked.has_value());
    EXPECT_TRUE(unmarked->mark_price.is_undef());
    EXPECT_TRUE(unmarked->index_price.is_undef());
}

TEST(OkxOptionGreeks, AZeroBidVolIsNoBidAndAnUnlistedOptionIsSkipped) {
    const auto loaded = load_catalogue();
    WireOptSummary row{};
    row.inst_id = "BTC-USD-261004-80000-C";
    row.delta_bs = "0.98";
    row.mark_vol = "0.5";
    row.bid_vol = "0";
    row.ask_vol = "0.6";
    const auto greeks = map_option_greeks(row, loaded->table, OptionMemo{}, UnixNanos{7});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_TRUE(greeks->bid_iv.is_undef());
    EXPECT_EQ(greeks->ask_iv, e9("0.6"));
    EXPECT_TRUE(greeks->vega.is_undef());
    EXPECT_EQ(greeks->ts_event.ns, 7U);

    row.inst_id = "BTC-USD-261004-1-C";
    EXPECT_FALSE(map_option_greeks(row, loaded->table, OptionMemo{}, UnixNanos{7}).has_value());
}

TEST(OkxOptionGreeks, AUsdMarginedFamilyStatesOneDeltaForBothConventions) {
    static SummaryBuffer rows;
    const auto frame = fixture("ws_opt_summary_um.json");
    const auto wire = parse_opt_summary(frame, rows);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 2U);
    EXPECT_TRUE((*wire)[0].inst_id.starts_with("BTC-USD_UM-"));
    EXPECT_EQ((*wire)[0].uly, "BTC-USD");
    EXPECT_FALSE((*wire)[0].delta_bs.empty());
}

TEST(OkxOptionStream, MarkIndexAndQuoteFramesParse) {
    const auto loaded = load_catalogue();
    const auto mark_frame = fixture("ws_mark_price_option.json");
    EXPECT_EQ(parse_event(mark_frame)->kind, EventKind::MarkPrice);
    const auto mark = parse_mark_price(mark_frame);
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->inst_id, "BTC-USD-261004-84900-C");
    EXPECT_EQ(parse_rate_e9(mark->mark_price), e9("0.001763557"));

    const auto index_frame = fixture("ws_index_tickers.json");
    EXPECT_EQ(parse_event(index_frame)->kind, EventKind::IndexPrice);
    const auto index = parse_index_ticker(index_frame);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(index->inst_id, "BTC-USD");
    EXPECT_EQ(index->index_price, "84910.1");

    const auto bbo_frame = fixture("ws_bbo_tbt_option.json");
    const auto bbo = parse_bbo(bbo_frame);
    ASSERT_TRUE(bbo.has_value());
    const auto quote = map_bbo(*bbo, loaded->table, UnixNanos{1});
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->bid.raw, 17);
    EXPECT_EQ(quote->ask.raw, 18);
    EXPECT_EQ(quote->bid_qty.raw, 1800);
    EXPECT_EQ(quote->ts_event.ns, 1'791'049'080'907'000'000ULL);

    const auto empty_frame = fixture("ws_bbo_tbt_option_empty.json");
    const auto empty = parse_bbo(empty_frame);
    EXPECT_FALSE(empty.has_value() && map_bbo(*empty, loaded->table, UnixNanos{1}).has_value());
}

TEST(OkxOptionStream, AFamilyPrintMapsOntoItsOption) {
    WireInstrument row{};
    row.inst_id = "ETH-USD-261005-2700-C";
    row.inst_type = "OPTION";
    row.state = "live";
    row.tick_size = "0.0001";
    row.lot_size = "1";
    row.ct_val = "1";
    row.ct_mult = "0.1";
    row.ct_type = "inverse";
    row.strike = "2700";
    row.opt_type = "C";
    row.expiry_ms = 1'791'187'200'000ULL;
    exec::InstrumentTable table;
    ASSERT_TRUE(table.add(*map_instrument(row, MarketType::Option)).has_value());

    const auto frame = fixture("ws_option_trades.json");
    EXPECT_EQ(parse_event(frame)->kind, EventKind::Trade);
    static TradeBuffer storage;
    const auto prints = parse_trades(frame, storage);
    ASSERT_TRUE(prints.has_value());
    ASSERT_EQ(prints->size(), 1U);
    const auto trade = map_trade((*prints)[0], table, UnixNanos{1});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 32);
    EXPECT_EQ(trade->qty.raw, 3);
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event.ns, 1'791'049'820'934'000'000ULL);
}

TEST(OkxOptionStream, FamiliesSubscribeByInstFamily) {
    EXPECT_EQ(option_family("BTC-USD-261004-84900-C"), "BTC-USD");
    EXPECT_EQ(option_family("ETH-USD_UM-261225-3000-P"), "ETH-USD_UM");
    EXPECT_EQ(option_family("BTC"), "");
    EXPECT_EQ(option_index("ETH-USD_UM"), "ETH-USD");
    EXPECT_EQ(option_index("BTC-USD"), "BTC-USD");

    Protocol protocol;
    const std::array<Subscription, 2> subs{
        Subscription{.channel = k_opt_summary_channel, .inst_family = "BTC-USD"},
        Subscription{.channel = k_option_trades_channel, .inst_type = "OPTION", .inst_family = "BTC-USD_UM"}};
    const auto frame = protocol.subscribe_frame(subs);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text,
              R"({"op":"subscribe","args":[{"channel":"opt-summary","instFamily":"BTC-USD"},)"
              R"({"channel":"option-trades","instType":"OPTION","instFamily":"BTC-USD_UM"}]})");
}

} // namespace
} // namespace raikiri::venues::okx
