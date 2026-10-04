#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/key.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/catalogue.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/market_data.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

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

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_BINANCE_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

FixedE9 e9(std::string_view text) {
    return *parse_decimal_e9(text);
}

constexpr std::string_view k_call = "BTC-261225-85000-C";
constexpr std::string_view k_put = "BTC-261225-85000-P";

struct Loaded {
    std::string body;
    std::size_t rows{0};
    exec::InstrumentTable table;
    std::vector<Instrument> catalogue;
};

std::unique_ptr<Loaded> load_catalogue() {
    auto out = std::make_unique<Loaded>();
    out->body = fixture("rest_option_exchange_info.json");
    static InstrumentBuffer storage;
    const auto rows = parse_option_exchange_info(out->body, storage);
    EXPECT_TRUE(rows.has_value());
    if (!rows.has_value()) {
        return out;
    }
    out->rows = rows->size();
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Option); instrument.has_value()) {
            classify(VenueId::Binance, *instrument);
            if (const auto id = out->table.add(*instrument); id.has_value()) {
                out->catalogue.push_back(*out->table.find(*id));
            }
        }
    }
    return out;
}

std::span<const WireOptionMark> marks() {
    static const auto frame = fixture("ws_option_mark.json");
    static OptionMarkBuffer storage;
    const auto data = unwrap(frame);
    EXPECT_TRUE(data.has_value());
    const auto rows = parse_option_marks(data.value_or(std::string_view{}), storage);
    EXPECT_TRUE(rows.has_value());
    return rows.value_or(std::span<const WireOptionMark>{});
}

const WireOptionMark& mark_of(std::string_view symbol) {
    for (const auto& row : marks()) {
        if (row.symbol == symbol) {
            return row;
        }
    }
    static const WireOptionMark none{};
    ADD_FAILURE() << "no mark row for " << symbol;
    return none;
}

TEST(BinanceOptionCatalogue, MapsTradingOptionsAndDropsClosedRowsAndStrikesOffTheTick) {
    const auto loaded = load_catalogue();
    // three btc, twenty doge and one closed xau
    EXPECT_EQ(loaded->rows, 24U);
    // twelve doge strikes such as 0.084 sit off the 0.01 premium tick
    ASSERT_EQ(loaded->catalogue.size(), 11U);
    EXPECT_EQ(loaded->table.find("DOGE-261009-0.084-C"), nullptr);
    EXPECT_EQ(loaded->table.find("XAU-261009-4075-C"), nullptr);

    const auto* found = loaded->table.find(k_call);
    ASSERT_NE(found, nullptr);
    const auto& call = std::get<OptionInstrument>(*found);
    EXPECT_EQ(call.put_call, PutCall::Call);
    EXPECT_EQ(call.premium, OptionUnit::Quote);
    EXPECT_EQ(call.greeks, OptionUnit::Quote);
    EXPECT_EQ(call.base.tick_size, e9("5"));
    EXPECT_EQ(call.base.lot_size, e9("0.01"));
    EXPECT_EQ(call.base.contract_value, e9("1"));
    EXPECT_EQ(call.base.underlying.view(), "BTC");
    EXPECT_EQ(call.base.asset_class, AssetClass::Crypto);
    const auto key = options::key_of(call);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "BTC-20261225-85000-C");
    EXPECT_EQ(call.expiry.ns % 86'400'000'000'000ULL, 8ULL * 3'600'000'000'000ULL);

    const auto& doge = std::get<OptionInstrument>(*loaded->table.find("DOGE-261009-0.09-P"));
    EXPECT_EQ(doge.base.contract_value, e9("1000"));
    EXPECT_EQ(options::canonical_name(*options::key_of(doge)), "DOGE-20261009-0.09-P");
}

TEST(BinanceOptionChain, PairsTheCallAndPutOfOneStrike) {
    const auto loaded = load_catalogue();
    const auto chain = options::OptionChain::build(loaded->catalogue);
    EXPECT_EQ(chain.size(), 11U);
    const auto* btc = chain.underlying("BTC");
    ASSERT_NE(btc, nullptr);
    ASSERT_EQ(btc->expiries.size(), 2U);
    const auto& december = btc->expiries[1];
    ASSERT_EQ(december.rows.size(), 1U);
    EXPECT_EQ(december.rows[0].strike, e9("85000"));
    ASSERT_TRUE(december.rows[0].call.has_value());
    ASSERT_TRUE(december.rows[0].put.has_value());
    EXPECT_EQ(loaded->table.exchange_symbol(*december.rows[0].call), k_call);
    EXPECT_EQ(loaded->table.exchange_symbol(*december.rows[0].put), k_put);

    const auto* doge = chain.underlying("DOGE");
    ASSERT_NE(doge, nullptr);
    ASSERT_EQ(doge->expiries.size(), 1U);
    ASSERT_EQ(doge->expiries[0].rows.size(), 4U);
    EXPECT_EQ(doge->expiries[0].rows[0].strike, e9("0.08"));
    EXPECT_EQ(doge->expiries[0].rows[3].strike, e9("0.11"));
}

TEST(BinanceOptionGreeks, KeepsTheVenueConventionWhichIsOurs) {
    const auto loaded = load_catalogue();
    EXPECT_EQ(marks().size(), 20U);
    const auto greeks = map_option_greeks(mark_of("DOGE-261009-0.1-P"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument, base_of(*loaded->table.find("DOGE-261009-0.1-P")).id);
    EXPECT_EQ(greeks->mark_iv, e9("0.69625616"));
    EXPECT_EQ(greeks->bid_iv, e9("0.61504364"));
    EXPECT_EQ(greeks->ask_iv, e9("0.77746868"));
    EXPECT_EQ(greeks->delta, e9("-0.78773325"));
    EXPECT_EQ(greeks->gamma, e9("39.09894638"));
    EXPECT_EQ(greeks->vega, e9("0.0000309"));
    EXPECT_EQ(greeks->theta, e9("-0.00022733"));
    EXPECT_EQ(greeks->mark_price, e9("7.2896"));
    EXPECT_EQ(greeks->index_price, e9("0.09357122"));
    EXPECT_TRUE(greeks->underlying_price.is_undef());
    EXPECT_EQ(greeks->ts_event.ns, 1'791'124'000'003'000'000ULL);

    // an ask iv of -1 says no ask
    const auto one_sided = map_option_greeks(mark_of("DOGE-261009-0.11-C"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(one_sided.has_value());
    EXPECT_TRUE(one_sided->ask_iv.is_undef());
    EXPECT_EQ(one_sided->bid_iv, e9("1.05963051"));

    EXPECT_FALSE(map_option_greeks(mark_of("DOGE-261009-0.084-P"), loaded->table, UnixNanos{1}).has_value());
}

TEST(BinanceOptionQuote, TheMarkRowQuotesUntilTheBookTickerSpeaks) {
    const auto loaded = load_catalogue();
    const auto quote = map_option_quote(mark_of("DOGE-261009-0.1-P"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->bid.raw, 705);
    EXPECT_EQ(quote->ask.raw, 755);
    EXPECT_EQ(quote->bid_qty.raw, 40'000);
    EXPECT_EQ(quote->ask_qty.raw, 3'001);

    const auto no_ask = map_option_quote(mark_of("DOGE-261009-0.11-C"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(no_ask.has_value());
    EXPECT_EQ(no_ask->bid.raw, 51);
    EXPECT_TRUE(no_ask->ask.is_undef());

    // a best bid of 4.7699 is off the 0.01 tick and is dropped, never rounded
    EXPECT_FALSE(map_option_quote(mark_of("DOGE-261009-0.09-C"), loaded->table, UnixNanos{1}).has_value());
}

TEST(BinanceOptionStream, BookTickerAndTradesMapOnTheOptionSymbol) {
    const auto loaded = load_catalogue();
    const auto ticker_frame = fixture("ws_option_book_ticker.json");
    const auto ticker_data = unwrap(ticker_frame);
    ASSERT_TRUE(ticker_data.has_value());
    const auto ticker = parse_book_ticker(*ticker_data);
    ASSERT_TRUE(ticker.has_value());
    const auto bbo = map_bbo(*ticker, loaded->table, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 1'278);
    EXPECT_EQ(bbo->ask.raw, 1'298);
    EXPECT_EQ(bbo->bid_qty.raw, 846);
    EXPECT_EQ(bbo->ts_event.ns, 1'791'123'980'077'000'000ULL);

    const auto trade_frame = fixture("ws_option_trade.json");
    const auto trade_data = unwrap(trade_frame);
    ASSERT_TRUE(trade_data.has_value());
    const auto event = parse_event_payload(*trade_data);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Trade);
    const auto wire = parse_trade(*trade_data);
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_trade(*wire, loaded->table, UnixNanos{1});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->instrument, base_of(*loaded->table.find("BTC-261030-86000-P")).id);
    EXPECT_EQ(trade->price.raw, 664);
    EXPECT_EQ(trade->qty.raw, 5);
    EXPECT_EQ(trade->aggressor, Side::Sell);
}

TEST(BinanceOptionStream, StreamsByUnderlyingOnTheOptionPaths) {
    std::array<char, 64> buf{};
    EXPECT_EQ(stream_name(exec::SubKind::Bbo, k_call, buf, MarketType::Option), "btc-261225-85000-c@bookTicker");
    EXPECT_EQ(stream_name(exec::SubKind::Trades, k_call, buf, MarketType::Option), "btcusdt@optionTrade");
    EXPECT_EQ(stream_name(exec::SubKind::Funding, k_call, buf, MarketType::Option), "btcusdt@optionMarkPrice");
    EXPECT_TRUE(stream_name(exec::SubKind::Depth, k_call, buf, MarketType::Option).empty());
    EXPECT_EQ(stream_name(exec::SubKind::Trades, "BTCUSDT", buf), "btcusdt@trade");
    EXPECT_EQ(stream_path(exec::SubKind::Bbo, MarketType::Option), "/public/stream");
    EXPECT_EQ(stream_path(exec::SubKind::Funding, MarketType::Option), "/market/stream");
    EXPECT_EQ(stream_path(exec::SubKind::Bbo), "/stream");
    EXPECT_EQ(ep::rest_host(false, MarketType::Option), "eapi.binance.com");

    net::Signer signer;
    RequestBuilder builder{signer, MarketType::Option};
    EXPECT_EQ(builder.instruments().target, "/eapi/v1/exchangeInfo");
    EXPECT_EQ(builder.server_time().target, "/eapi/v1/time");
}

} // namespace
} // namespace raikiri::venues::binance
