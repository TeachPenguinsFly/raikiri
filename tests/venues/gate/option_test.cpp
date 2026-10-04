#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/key.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/catalogue.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::gate {
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

constexpr std::string_view k_call = "BTC-20261225-90000-C";
constexpr std::string_view k_put = "BTC-20261225-90000-P";

struct Loaded {
    std::string body;
    std::size_t rows{0};
    exec::InstrumentTable table;
    std::vector<Instrument> catalogue;
};

std::unique_ptr<Loaded> load_catalogue() {
    auto out = std::make_unique<Loaded>();
    out->body = fixture("rest_option_contracts.json");
    static OptionContractBuffer storage;
    const auto rows = parse_option_contracts(out->body, storage);
    EXPECT_TRUE(rows.has_value());
    if (!rows.has_value()) {
        return out;
    }
    out->rows = rows->size();
    for (const auto& row : *rows) {
        if (auto instrument = map_option_contract(row); instrument.has_value()) {
            classify(VenueId::Gate, *instrument);
            if (const auto id = out->table.add(*instrument); id.has_value()) {
                out->catalogue.push_back(*out->table.find(*id));
            }
        }
    }
    return out;
}

TEST(GateOptionCatalogue, HoldsNamesWithoutTheQuoteSoSixDigitStrikesFit) {
    EXPECT_EQ(option_symbol("BTC_USDT-20261225-100000-C")->view(), "BTC-20261225-100000-C");
    EXPECT_EQ(option_symbol("DOGE_USDT-20261006-0.102-P")->view(), "DOGE-20261006-0.102-P");
    EXPECT_FALSE(option_symbol("BTC_USD-20261225-100000-C").has_value());
    EXPECT_FALSE(option_symbol("BTC_USDT").has_value());
    EXPECT_EQ(option_underlying("BTC-20261225-100000-C"), "BTC_USDT");

    const auto listing = fixture("rest_option_underlyings.json");
    OptionUnderlyingBuffer names;
    const auto underlyings = parse_option_underlyings(listing, names);
    ASSERT_TRUE(underlyings.has_value());
    ASSERT_EQ(underlyings->size(), 12U);
    EXPECT_EQ((*underlyings)[0], "BTC_USDT");
}

TEST(GateOptionCatalogue, MapsContractsWithTheMultiplierAsContractValue) {
    const auto loaded = load_catalogue();
    EXPECT_EQ(loaded->rows, 6U);
    ASSERT_EQ(loaded->catalogue.size(), 6U);
    ASSERT_NE(loaded->table.find("BTC-20261225-100000-C"), nullptr);

    const auto* found = loaded->table.find(k_call);
    ASSERT_NE(found, nullptr);
    const auto& call = std::get<OptionInstrument>(*found);
    EXPECT_EQ(call.put_call, PutCall::Call);
    EXPECT_EQ(call.premium, OptionUnit::Quote);
    EXPECT_EQ(call.greeks, OptionUnit::Quote);
    EXPECT_EQ(call.base.tick_size, e9("1"));
    EXPECT_EQ(call.base.lot_size, e9("1"));
    EXPECT_EQ(call.base.contract_value, e9("0.01"));
    EXPECT_EQ(call.base.underlying.view(), "BTC");
    const auto key = options::key_of(call);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "BTC-20261225-90000-C");
    EXPECT_EQ(call.expiry.ns % 86'400'000'000'000ULL, 8ULL * 3'600'000'000'000ULL);

    const auto& doge = std::get<OptionInstrument>(*loaded->table.find("DOGE-20261006-0.102-P"));
    EXPECT_EQ(doge.base.contract_value, e9("1000"));
    EXPECT_EQ(options::canonical_name(*options::key_of(doge)), "DOGE-20261006-0.102-P");

    WireOptionContract inactive{};
    inactive.name = "BTC_USDT-20261225-90000-C";
    inactive.strike_price = "90000";
    inactive.order_price_round = "1";
    inactive.multiplier = "0.01";
    inactive.expiration_s = 1'798'185'600;
    EXPECT_FALSE(map_option_contract(inactive).has_value());
    inactive.is_active = true;
    inactive.strike_price = "90000.5";
    EXPECT_FALSE(map_option_contract(inactive).has_value());
}

TEST(GateOptionChain, PairsTheCallAndPutOfOneStrike) {
    const auto loaded = load_catalogue();
    const auto chain = options::OptionChain::build(loaded->catalogue);
    EXPECT_EQ(chain.size(), 6U);
    const auto* btc = chain.underlying("BTC");
    ASSERT_NE(btc, nullptr);
    ASSERT_EQ(btc->expiries.size(), 3U);
    const auto& december = btc->expiries[1];
    ASSERT_EQ(december.rows.size(), 2U);
    EXPECT_EQ(december.rows[0].strike, e9("90000"));
    ASSERT_TRUE(december.rows[0].call.has_value());
    ASSERT_TRUE(december.rows[0].put.has_value());
    EXPECT_EQ(loaded->table.exchange_symbol(*december.rows[0].call), k_call);
    EXPECT_EQ(loaded->table.exchange_symbol(*december.rows[0].put), k_put);
    EXPECT_FALSE(december.rows[1].put.has_value());
    ASSERT_NE(chain.underlying("DOGE"), nullptr);
}

TEST(GateOptionGreeks, KeepsTheVenueConventionWhichIsOurs) {
    const auto loaded = load_catalogue();
    const auto frame = fixture("ws_option_contract_tickers.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Ticker);
    const auto wire = parse_option_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->contract, "BTC_USDT-20261225-90000-C");

    const auto greeks = map_option_greeks(*wire, loaded->table, UnixNanos{1});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument, base_of(*loaded->table.find(k_call)).id);
    EXPECT_EQ(greeks->delta, e9("0.4328"));
    EXPECT_EQ(greeks->gamma, e9("0.000026"));
    EXPECT_EQ(greeks->vega, e9("160.29846"));
    EXPECT_EQ(greeks->theta, e9("-35.63878"));
    EXPECT_EQ(greeks->rho, e9("73.85777"));
    EXPECT_EQ(greeks->mark_iv, e9("0.3634"));
    EXPECT_EQ(greeks->bid_iv, e9("0.3571"));
    EXPECT_EQ(greeks->ask_iv, e9("0.3732"));
    EXPECT_EQ(greeks->underlying_price, e9("86134.55"));
    EXPECT_EQ(greeks->index_price, e9("85181.59"));
    EXPECT_EQ(greeks->mark_price, e9("4249.2"));
    EXPECT_EQ(greeks->ts_event.ns, 1'791'123'552'000'000'000ULL);

    auto no_bid = *wire;
    no_bid.bid_iv = "0";
    no_bid.bid_qty = "0";
    no_bid.bid_price = "0";
    const auto one_sided = map_option_greeks(no_bid, loaded->table, UnixNanos{1});
    ASSERT_TRUE(one_sided.has_value());
    EXPECT_TRUE(one_sided->bid_iv.is_undef());

    const auto quote = map_option_quote(*wire, loaded->table, UnixNanos{1});
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->bid.raw, 4'196);
    EXPECT_EQ(quote->ask.raw, 4'454);
    EXPECT_EQ(quote->bid_qty.raw, 200);
    EXPECT_EQ(quote->ask_qty.raw, 416);
    const auto empty_bid = map_option_quote(no_bid, loaded->table, UnixNanos{1});
    ASSERT_TRUE(empty_bid.has_value());
    EXPECT_TRUE(empty_bid->bid.is_undef());
    EXPECT_EQ(empty_bid->ask.raw, 4'454);
}

TEST(GateOptionStream, BookTickerAndTradesMapOnTheHeldName) {
    const auto loaded = load_catalogue();
    const auto book_frame = fixture("ws_option_book_ticker.json");
    ASSERT_EQ(parse_event(book_frame)->kind, EventKind::BookTicker);
    auto ticker = parse_book_ticker(book_frame);
    ASSERT_TRUE(ticker.has_value());
    const auto held = option_symbol(ticker->contract);
    ASSERT_TRUE(held.has_value());
    ticker->contract = held->view();
    const auto bbo = map_bbo(*ticker, loaded->table, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->instrument, base_of(*loaded->table.find("BTC-20270326-35000-C")).id);
    EXPECT_EQ(bbo->bid.raw, 46'145);
    EXPECT_EQ(bbo->ask.raw, 89'481);
    EXPECT_EQ(bbo->bid_qty.raw, 70);
    EXPECT_EQ(bbo->ts_event.ns, 1'791'123'575'318'000'000ULL);

    const auto trade_frame = fixture("ws_option_trades.json");
    ASSERT_EQ(parse_event(trade_frame)->kind, EventKind::Trade);
    TradeBuffer storage;
    const auto trades = parse_trades(trade_frame, storage, MarketType::Option);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    auto print = (*trades)[0];
    const auto traded = option_symbol(print.contract);
    ASSERT_TRUE(traded.has_value());
    print.contract = traded->view();
    const auto trade = map_trade(print, loaded->table, UnixNanos{1});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->instrument, base_of(*loaded->table.find("BTC-20261005-83500-P")).id);
    EXPECT_EQ(trade->price.raw, 2);
    EXPECT_EQ(trade->qty.raw, 2);
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->ts_event.ns, 1'791'123'735'212'000'000ULL);
}

TEST(GateOptionStream, SubscribesByUnderlyingOnTheOptionHost) {
    EXPECT_EQ(channel_for(exec::SubKind::Bbo, MarketType::Option), "options.book_ticker");
    EXPECT_EQ(channel_for(exec::SubKind::Trades, MarketType::Option), "options.trades");
    EXPECT_EQ(channel_for(exec::SubKind::Funding, MarketType::Option), "options.contract_tickers");
    EXPECT_TRUE(channel_for(exec::SubKind::Depth, MarketType::Option).empty());
    EXPECT_EQ(public_host(false, MarketType::Option), "op-ws.gateio.live");
    EXPECT_EQ(public_path(false, MarketType::Option), "/v4/ws");
    EXPECT_EQ(public_host(false, MarketType::Perp), "fx-ws.gateio.ws");

    Protocol protocol;
    const std::array<Subscription, 2> subs{Subscription{.channel = "options.book_ticker", .contract = "BTC_USDT"},
                                           Subscription{.channel = "options.book_ticker", .contract = "ETH_USDT"}};
    const auto frame = protocol.subscribe_frame(subs, 1'791'123'500);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(
        frame->text,
        R"({"time":1791123500,"channel":"options.book_ticker","event":"subscribe","payload":{"underlyings":["BTC_USDT","ETH_USDT"]}})");
    EXPECT_EQ(protocol.ping_frame(1'791'123'500, MarketType::Option),
              R"({"time":1791123500,"channel":"options.ping"})");

    RequestBuilder builder;
    EXPECT_EQ(builder.option_contracts("BTC_USDT").target, "/api/v4/options/contracts?underlying=BTC_USDT");
    EXPECT_EQ(builder.option_underlyings().target, "/api/v4/options/underlyings");
}

} // namespace
} // namespace raikiri::venues::gate
