#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/key.hpp>
#include <raikiri/venues/bybit/book.hpp>
#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/market_data.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::bybit {
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

constexpr std::string_view k_call = "BTC-26MAR27-90000-C-USDT";
constexpr std::string_view k_put = "BTC-26MAR27-90000-P-USDT";

struct Loaded {
    std::string body;
    std::size_t rows{0};
    exec::InstrumentTable table;
    std::vector<Instrument> catalogue;
};

std::unique_ptr<Loaded> load_catalogue() {
    auto out = std::make_unique<Loaded>();
    out->body = fixture("rest_instruments_option.json");
    const auto envelope = parse_envelope(out->body);
    EXPECT_TRUE(envelope.has_value());
    if (!envelope.has_value()) {
        return out;
    }
    static InstrumentBuffer storage;
    const auto rows = parse_instruments(envelope->result, storage);
    EXPECT_TRUE(rows.has_value());
    if (!rows.has_value()) {
        return out;
    }
    out->rows = rows->size();
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Option); instrument.has_value()) {
            classify(VenueId::Bybit, *instrument);
            if (const auto id = out->table.add(*instrument); id.has_value()) {
                out->catalogue.push_back(*out->table.find(*id));
            }
        }
    }
    return out;
}

TEST(BybitOptionCatalogue, MapsUsdtOptionsAndDropsNamesPastTheSymbolWidth) {
    const auto loaded = load_catalogue();
    EXPECT_EQ(loaded->rows, 4U);
    // BTC-25JUN27-106000-P-USDT is 25 bytes, one past SymbolText
    ASSERT_EQ(loaded->catalogue.size(), 2U);

    const auto* found = loaded->table.find(k_call);
    ASSERT_NE(found, nullptr);
    const auto& call = std::get<OptionInstrument>(*found);
    EXPECT_EQ(call.put_call, PutCall::Call);
    EXPECT_EQ(call.premium, OptionUnit::Quote);
    EXPECT_EQ(call.greeks, OptionUnit::Quote);
    EXPECT_EQ(call.base.tick_size, e9("5"));
    EXPECT_EQ(call.base.lot_size, e9("0.01"));
    EXPECT_EQ(call.base.underlying.view(), "BTC");
    EXPECT_EQ(call.base.id.market(), MarketType::Option);
    const auto key = options::key_of(call);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "BTC-20270326-90000-C");
    EXPECT_EQ(call.expiry.ns % 86'400'000'000'000ULL, 8ULL * 3'600'000'000'000ULL);
}

TEST(BybitOptionCatalogue, ReadsAFractionalStrikeFromTheSymbol) {
    WireInstrument row{};
    row.symbol = "XRP-4OCT26-1.1-C-USDT";
    row.base_coin = "XRP";
    row.quote_coin = "USDT";
    row.status = "Trading";
    row.tick_size = "0.0001";
    row.qty_step = "10";
    row.options_type = "Call";
    row.delivery_ms = 1'791'100'800'000ULL;
    const auto mapped = map_instrument(row, MarketType::Option);
    ASSERT_TRUE(mapped.has_value());
    auto instrument = *mapped;
    classify(VenueId::Bybit, instrument);
    const auto key = options::key_of(std::get<OptionInstrument>(instrument));
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "XRP-20261004-1.1-C");

    row.options_type = "";
    EXPECT_FALSE(map_instrument(row, MarketType::Option).has_value());
    row.options_type = "Put";
    row.symbol = "XRP-4OCT26-1.10005-P-USDT";
    EXPECT_FALSE(map_instrument(row, MarketType::Option).has_value());
}

TEST(BybitOptionChain, PairsTheCallAndPutOfOneStrike) {
    const auto loaded = load_catalogue();
    const auto chain = options::OptionChain::build(loaded->catalogue);
    EXPECT_EQ(chain.size(), 2U);
    const auto* btc = chain.underlying("BTC");
    ASSERT_NE(btc, nullptr);
    ASSERT_EQ(btc->expiries.size(), 1U);
    ASSERT_EQ(btc->expiries[0].rows.size(), 1U);
    const auto& row = btc->expiries[0].rows[0];
    EXPECT_EQ(row.strike, e9("90000"));
    ASSERT_TRUE(row.call.has_value());
    ASSERT_TRUE(row.put.has_value());
    EXPECT_EQ(loaded->table.exchange_symbol(*row.call), k_call);
    EXPECT_EQ(loaded->table.exchange_symbol(*row.put), k_put);
}

TEST(BybitOptionGreeks, KeepsTheVenueConventionWhichIsOurs) {
    const auto loaded = load_catalogue();
    const auto frame = fixture("option_tickers.json");
    const auto topic = parse_topic(frame);
    ASSERT_TRUE(topic.has_value());
    EXPECT_EQ(topic->kind, TopicKind::Tickers);

    const auto wire = parse_option_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, k_call);
    const auto greeks = map_option_greeks(*wire, loaded->table, UnixNanos{1});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument, base_of(*loaded->table.find(k_call)).id);
    EXPECT_EQ(greeks->delta, e9("0.49801447"));
    EXPECT_EQ(greeks->gamma, e9("0.00001782"));
    EXPECT_EQ(greeks->vega, e9("239.22969112"));
    EXPECT_EQ(greeks->theta, e9("-25.72991624"));
    EXPECT_EQ(greeks->mark_iv, e9("0.3735"));
    EXPECT_EQ(greeks->bid_iv, e9("0.372"));
    EXPECT_EQ(greeks->ask_iv, e9("0.3787"));
    EXPECT_EQ(greeks->underlying_price, e9("86953"));
    EXPECT_EQ(greeks->index_price, e9("84899.07664735"));
    EXPECT_EQ(greeks->mark_price, e9("7621.88934212"));
    EXPECT_EQ(greeks->ts_event.ns, 1'791'049'140'169'000'000ULL);

    auto no_bid = *wire;
    no_bid.bid_iv = "0";
    no_bid.delta = "";
    const auto empty_side = map_option_greeks(no_bid, loaded->table, UnixNanos{1});
    ASSERT_TRUE(empty_side.has_value());
    EXPECT_TRUE(empty_side->bid_iv.is_undef());
    EXPECT_TRUE(empty_side->delta.is_undef());

    no_bid.symbol = "BTC-26MAR27-1-C-USDT";
    EXPECT_FALSE(map_option_greeks(no_bid, loaded->table, UnixNanos{1}).has_value());
}

TEST(BybitOptionStream, QuotesFromTheTwentyFiveLevelBook) {
    const auto loaded = load_catalogue();
    const auto frame = fixture("option_orderbook_snapshot.json");
    static OrderbookBuffer storage;
    const auto book = parse_orderbook(frame, storage);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->symbol, k_call);
    const auto& base = base_of(*loaded->table.find(k_call));
    BookSync sync{base.id, base.tick_size, base.lot_size};
    EXPECT_EQ(sync.apply(*book), SyncResult::Resynced);
    const auto bbo = sync.bbo(UnixNanos{2}, UnixNanos{3});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 1518);
    EXPECT_EQ(bbo->ask.raw, 1550);
    EXPECT_EQ(bbo->bid_qty.raw, 174);
}

TEST(BybitOptionStream, TradesArriveOnTheBaseCoinTopic) {
    const auto frame = fixture("option_public_trade.json");
    static TradeBuffer storage;
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ((*trades)[0].symbol, "BTC-4OCT26-85750-C-USDT");
    EXPECT_EQ((*trades)[0].price, "20");
    EXPECT_TRUE((*trades)[0].buyer_aggressor);

    std::array<char, 64> buf{};
    EXPECT_EQ(topic_name(exec::SubKind::Trades, "BTC", buf), "publicTrade.BTC");
    EXPECT_EQ(topic_name(exec::SubKind::Bbo, k_call, buf, k_option_book_depth),
              "orderbook.25.BTC-26MAR27-90000-C-USDT");
    EXPECT_EQ(public_stream_path(MarketType::Option), "/v5/public/option");
}

} // namespace
} // namespace raikiri::venues::bybit
