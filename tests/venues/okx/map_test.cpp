#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/okx/account.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/positions.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::okx {
namespace {

// only the wire target has RAIKIRI_OKX_FIXTURES, so this locates the directory itself
std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// BTC-USDT-SWAP as OKX publishes it: 0.1 tick, 0.01 lot, 0.01 BTC a contract.
exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.state = "live";
    wire.tick_size = "0.1";
    wire.lot_size = "0.01";
    wire.min_size = "0.01";
    wire.ct_val = "0.01";
    wire.ct_mult = "1";
    wire.ct_type = "linear";
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

constexpr std::string_view k_ulid = "01ARZ3NDEKTSV4RRFFQ69G5FAV";

// a 5-contract ioc on BTC-USDT-SWAP with 2 filled
WireOrder partly_filled_order() {
    WireOrder wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.ord_id = "680800019749904385";
    wire.cl_ord_id = k_ulid;
    wire.side = "buy";
    wire.ord_type = "ioc";
    wire.px = "64043.5";
    wire.sz = "5";
    wire.acc_fill_sz = "2";
    wire.avg_px = "64043.5";
    wire.fill_px = "64043.5";
    wire.fill_sz = "2";
    wire.fill_fee = "-0.0384261";
    wire.trade_id = "744876981";
    wire.exec_type = "T";
    wire.state = WireOrderState::PartiallyFilled;
    wire.fill_ms = 1'786'392'410'701ULL;
    wire.u_ms = 1'786'392'410'702ULL;
    return wire;
}

TEST(OkxMap, BboBecomesTopOfBook) {
    const auto table = table_with_btc();
    WireBbo wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.bid_price = "64043.5";
    wire.bid_qty = "298.67";
    wire.ask_price = "64043.6";
    wire.ask_qty = "1282.19";
    wire.ts_ms = 1'786'392'410'701ULL;

    const auto bbo = map_bbo(wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 640'435); // 64043.5 / 0.1
    EXPECT_EQ(bbo->ask.raw, 640'436);
    EXPECT_EQ(bbo->bid_qty.raw, 29'867); // contracts / 0.01
    EXPECT_EQ(bbo->ask_qty.raw, 128'219);
    EXPECT_EQ(bbo->ts_event.ns, 1'786'392'410'701ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(OkxMap, PriceOffTheTickIsDroppedNotRounded) {
    // a price off the tick is refused
    const auto table = table_with_btc();
    WireBbo wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.bid_price = "64043.55"; // 0.05 past a 0.1 tick
    wire.bid_qty = "1.00";
    wire.ask_price = "64043.6";
    wire.ask_qty = "1.00";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(OkxMap, OneSidedBookIsDropped) {
    // an empty side must not become a zero price
    const auto table = table_with_btc();
    WireBbo wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.ask_price = "64043.6";
    wire.ask_qty = "1.00";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(OkxMap, UnknownInstrumentIsDropped) {
    const auto table = table_with_btc();
    WireBbo wire{};
    wire.inst_id = "DOGE-USDT-SWAP";
    wire.bid_price = "0.1";
    wire.bid_qty = "1.00";
    wire.ask_price = "0.2";
    wire.ask_qty = "1.00";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(OkxMap, TakerSideIsCarriedThroughUnflipped) {
    // okx states the aggressor's side, so nothing is inverted
    const auto table = table_with_btc();
    WireTrade wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.price = "64043.5";
    wire.qty = "0.01";
    wire.ts_ms = 1'786'392'411'634ULL;

    wire.aggressor_bought = false;
    auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);

    wire.aggressor_bought = true;
    trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 640'435);
    EXPECT_EQ(trade->qty.raw, 1);
}

TEST(OkxMap, MissingVenueTimeFallsBackToIngest) {
    const auto table = table_with_btc();
    WireTrade wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.price = "64043.5";
    wire.qty = "0.01";
    wire.ts_ms = 0;
    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // zero timestamps fall back to ingest time
    EXPECT_EQ(trade->ts_event.ns, k_ingest.ns);
}

TEST(OkxMap, FundingUsesFundingTimeAndDerivesTheInterval) {
    FundingMemo memo{};
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = "0.0001000000000000";
    wire.funding_ms = 1'786'406'400'000ULL;
    wire.next_funding_ms = 1'786'435'200'000ULL;
    wire.ts_ms = 1'786'392'377'160ULL;

    const auto funding = map_funding(wire, table, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 0.0001 * 1e9
    // fundingTime, not nextFundingTime
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'406'400'000ULL * 1'000'000ULL);
    // the gap between the two gives the interval; not every contract runs 8 h
    EXPECT_EQ(funding->interval_s, 28'800U);
    // mark arrives on its own channel
    EXPECT_TRUE(funding->mark_price.is_undef());
}

TEST(OkxMap, ARateWithMorePrecisionThanWeCarryStillArrives) {
    FundingMemo memo{};
    // okx pads the rate to sixteen fractional digits
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = "0.0000439269860429";
    wire.funding_ms = 1'786'406'400'000ULL;

    const auto funding = map_funding(wire, table, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // Exact through 1e-9, and the dust below it is dropped toward zero.
    EXPECT_EQ(funding->rate.raw, 43'926);
}

TEST(OkxMap, EmptyForecastIsNotAPredictionOfZero) {
    FundingMemo memo{};
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = "0.0001";
    wire.next_funding_rate = {}; // shipped empty, not omitted
    wire.funding_ms = 1'786'406'400'000ULL;
    const auto funding = map_funding(wire, table, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_TRUE(funding->predicted_rate.is_undef());
}

TEST(OkxMap, ContractWithoutFundingIsNotReportedAsZero) {
    FundingMemo memo{};
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = {}; // absent, not "0"
    EXPECT_FALSE(map_funding(wire, table, memo, k_ingest).has_value());
}

TEST(OkxMap, SwapTurnoverIsTheBaseAssetAndNeverTheContractCount) {
    const auto table = table_with_btc();
    // held: parse_ticker returns views into it
    const auto frame = fixture("ws_tickers.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());

    const auto stats = map_ticker_stats(*wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    // volCcy24h is base; vol24h on this frame counts contracts
    EXPECT_EQ(stats->volume_24h_base.raw, 134'464'609'600'000LL);
    // 134 464.6096 x 79 326.8 is 10.67e9 USDT, past what an e9 column holds.
    EXPECT_EQ(stats->volume_24h_quote.raw, 10'666'647'192'817'280LL);
    EXPECT_EQ(stats->ts_event.ns, 1'788'532'273'172ULL * 1'000'000ULL);
}

TEST(OkxMap, SwapQuoteTurnoverIsTheBaseAtTheLastPrint) {
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.vol_24h = "7922892.8";
    wire.vol_ccy_24h = "79228.928";
    wire.last = "84555.3";
    const auto stats = map_ticker_stats(wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 79'228'928'000'000LL);
    EXPECT_EQ(stats->volume_24h_quote.raw, 6'699'225'775'718'400LL);

    wire.last = {};
    const auto unpriced = map_ticker_stats(wire, table, k_ingest);
    ASSERT_TRUE(unpriced.has_value());
    EXPECT_TRUE(unpriced->volume_24h_quote.is_undef());
}

TEST(OkxMap, SwapWhoseCoinCountOverflowsIsPricedByTheContract) {
    exec::InstrumentTable table;
    WireInstrument row{};
    row.inst_id = "PEPE-USDT-SWAP";
    row.inst_type = "SWAP";
    row.state = "live";
    row.tick_size = "0.000000001";
    row.lot_size = "0.1";
    row.min_size = "0.1";
    row.ct_val = "10000000";
    row.ct_mult = "1";
    row.ct_type = "linear";
    const auto pepe = map_instrument(row, MarketType::Perp);
    ASSERT_TRUE(pepe.has_value());
    ASSERT_TRUE(table.add(*pepe).has_value());

    const auto frame = fixture("ws_tickers_pepe.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_ticker_stats(*wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_TRUE(stats->volume_24h_base.is_undef());
    // 2 287 413.2 contracts x 10 000 000 PEPE x 0.000004406.
    EXPECT_EQ(stats->volume_24h_quote.raw, 100'783'425'592'000LL);
}

TEST(OkxMap, InverseSwapQuoteTurnoverIsContractsTimesTheirDollarValue) {
    exec::InstrumentTable table;
    WireInstrument row{};
    row.inst_id = "BTC-USD-SWAP";
    row.inst_type = "SWAP";
    row.state = "live";
    row.tick_size = "0.1";
    row.lot_size = "1";
    row.min_size = "1";
    row.ct_val = "100";
    row.ct_mult = "1";
    row.ct_type = "inverse";
    const auto inverse = map_instrument(row, MarketType::Perp);
    ASSERT_TRUE(inverse.has_value());
    ASSERT_TRUE(table.add(*inverse).has_value());

    WireTicker wire{};
    wire.inst_id = "BTC-USD-SWAP";
    wire.inst_type = "SWAP";
    wire.vol_24h = "7922892";
    wire.vol_ccy_24h = "9370.2";
    wire.last = "84555.3";
    const auto stats = map_ticker_stats(wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 9'370'200'000'000LL);
    EXPECT_EQ(stats->volume_24h_quote.raw, 792'289'200'000'000LL);
}

TEST(OkxMap, SpotTurnoverReadsTheSameTwoNamesTheOtherWayRound) {
    exec::InstrumentTable table;
    WireInstrument row{};
    row.inst_id = "BTC-USDT";
    row.inst_type = "SPOT";
    row.state = "live";
    row.base_ccy = "BTC";
    row.quote_ccy = "USDT";
    row.tick_size = "0.1";
    row.lot_size = "0.00000001";
    row.min_size = "0.00001";
    const auto spot = map_instrument(row, MarketType::Spot);
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(table.add(*spot).has_value());

    // held: parse_ticker returns views into it
    const auto frame = fixture("ws_tickers_spot.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_ticker_stats(*wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    // on spot vol24h is base and volCcy24h quote
    EXPECT_EQ(stats->volume_24h_base.raw, 10'451'543'743'020LL);
    EXPECT_EQ(stats->volume_24h_quote.raw, 843'192'320'028'134LL);
}

TEST(OkxMap, ATickerStatingNoVolumeIsNotAMarketAtZero) {
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.ts_ms = 1'788'532'273'172ULL;
    // neither column stated
    EXPECT_FALSE(map_ticker_stats(wire, table, k_ingest).has_value());

    // an unloaded instrument has no cell to write to
    wire.vol_ccy_24h = "134464.6096";
    wire.inst_id = "NOT-LISTED-SWAP";
    EXPECT_FALSE(map_ticker_stats(wire, table, k_ingest).has_value());
}

TEST(OkxMap, ContractValueComesFromTheCatalogueNotFromOne) {
    // a swap quotes in contracts, so contract_value is ctVal
    WireInstrument wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.state = "live";
    wire.tick_size = "0.1";
    wire.lot_size = "0.01";
    wire.ct_val = "0.01";
    wire.ct_mult = "1";
    wire.ct_type = "linear";
    auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 10'000'000); // 0.01 * 1e9
    EXPECT_EQ(base_of(*instrument).kind, ContractKind::Linear);

    // the inverse contract is 100 usd; ctType alone separates the two
    wire.inst_id = "BTC-USD-SWAP";
    wire.ct_val = "100";
    wire.ct_mult = "2";
    wire.ct_type = "inverse";
    instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 200 * k_e9);
    EXPECT_EQ(base_of(*instrument).kind, ContractKind::Inverse);

    // a fractional multiplier is refused
    wire.ct_mult = "1.5";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(OkxMap, NonLiveAndWrongMarketRowsAreRefused) {
    // a preopen row ships numeric fields empty, so state is checked first
    WireInstrument wire{};
    wire.inst_id = "JP225-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.state = "preopen";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    // perps were asked for, so a dated future is refused
    wire.inst_id = "BTC-USD-260814";
    wire.inst_type = "FUTURES";
    wire.state = "live";
    wire.tick_size = "0.1";
    wire.lot_size = "0.1";
    wire.ct_val = "100";
    wire.ct_mult = "1";
    wire.ct_type = "inverse";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(OkxMap, SwapThatWillNotSayItsDirectionIsRefused) {
    // a swap row without ctType cannot be priced and is refused
    WireInstrument wire{};
    wire.inst_id = "BTC-USD-SWAP";
    wire.inst_type = "SWAP";
    wire.state = "live";
    wire.tick_size = "0.1";
    wire.lot_size = "0.1";
    wire.ct_val = "100";
    wire.ct_mult = "1";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.ct_type = "futures"; // a value we do not know how to price
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.ct_type = "inverse";
    EXPECT_TRUE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(OkxMap, InstrumentWithoutUsableStepsIsRefused) {
    // a zero step must never reach the table
    WireInstrument wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.state = "live";
    wire.tick_size = "0";
    wire.lot_size = "0.01";
    wire.ct_val = "0.01";
    wire.ct_mult = "1";
    wire.ct_type = "linear";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.tick_size = "0.1";
    wire.lot_size = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.lot_size = "0.01";
    wire.ct_val = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(OkxMap, CatalogueSwapRowsTakeTheirPairFromInstId) {
    auto storage = std::make_unique<InstrumentBuffer>();
    const auto body = fixture("rest_instruments.json");
    const auto rows = parse_instruments(body, *storage);
    ASSERT_TRUE(rows.has_value());

    std::size_t mapped = 0;
    for (const auto& row : *rows) {
        EXPECT_TRUE(row.base_ccy.empty() && row.quote_ccy.empty())
            << row.inst_id << ": OKX leaves the pair fields empty off SPOT";
        const auto instrument = map_instrument(row, MarketType::Perp);
        if (!instrument.has_value()) {
            continue;
        }
        const auto& base = base_of(*instrument);
        EXPECT_EQ(base.exchange_symbol.view(), row.inst_id);
        if (row.inst_id == "BTC-USDT-SWAP") {
            EXPECT_EQ(base.local_symbol.view(), "BTC/USDT");
        } else if (row.inst_id == "ETH-USDT-SWAP") {
            EXPECT_EQ(base.local_symbol.view(), "ETH/USDT");
        }
        ++mapped;
    }
    EXPECT_EQ(mapped, 2U) << "JP225-USDT-SWAP is preopen and BTC-USD-260814 is dated";
}

TEST(OkxMap, SpotRowTakesItsPairFromTheFields) {
    // spot is the one type that fills baseCcy and quoteCcy
    WireInstrument wire{};
    wire.inst_id = "BTC-USDT";
    wire.inst_type = "SPOT";
    wire.state = "live";
    wire.base_ccy = "BTC";
    wire.quote_ccy = "USDT";
    wire.tick_size = "0.1";
    wire.lot_size = "0.00000001";
    const auto instrument = map_instrument(wire, MarketType::Spot);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base_of(*instrument).exchange_symbol.view(), "BTC-USDT");
}

TEST(OkxMap, RowWithoutAPairIsDropped) {
    // a swap's pair comes only from instId; one that will not split is dropped
    WireInstrument wire{};
    wire.inst_id = "BTCUSDTSWAP";
    wire.inst_type = "SWAP";
    wire.state = "live";
    wire.tick_size = "0.1";
    wire.lot_size = "0.01";
    wire.ct_val = "0.01";
    wire.ct_mult = "1";
    wire.ct_type = "linear";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.inst_id = "BTC-USDT-SWAP";
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).local_symbol.view(), "BTC/USDT");
}

TEST(OkxMap, AFillCarriesItsTradeAndTheFeeSignFlips) {
    const auto table = table_with_btc();
    const auto fill = map_order_fill(partly_filled_order(), table, k_ingest);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->side, Side::Buy);
    EXPECT_EQ(fill->liquidity, Liquidity::Taker);
    EXPECT_EQ(fill->price.raw, 640'435); // 64043.5 / 0.1
    EXPECT_EQ(fill->qty.raw, 200);       // 2 contracts / 0.01
    // sz minus accFillSz is the only source of leaves
    EXPECT_EQ(fill->leaves_qty.raw, 300);
    EXPECT_EQ(fill->exec_id.view(), "744876981");
    EXPECT_EQ(fill->exchange_client_order_id.view(), k_ulid);
    // negative on the wire is a fee charged
    EXPECT_EQ(fill->fee.raw, 38'426'100);
    EXPECT_EQ(fill->ts_event.ns, 1'786'392'410'701ULL * 1'000'000ULL);

    // a partial fill carries no lifecycle transition of its own
    EXPECT_FALSE(map_order_event(partly_filled_order(), table, k_ingest).has_value());
}

TEST(OkxMap, AFilledStateWithoutATradeIsNotASecondFill) {
    // a filled push with fillSz 0 closes a spot market order and is not a second fill
    const auto table = table_with_btc();
    auto wire = partly_filled_order();
    wire.state = WireOrderState::Filled;
    wire.acc_fill_sz = "5";
    wire.trade_id = {};
    wire.fill_sz = "0";
    wire.fill_px = {};

    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());
    EXPECT_FALSE(map_order_event(wire, table, k_ingest).has_value());
}

TEST(OkxMap, OneMessageIsTheLastFillAndThenTheExpiry) {
    // a partly filled ioc that was killed arrives as one object; fill first
    const auto table = table_with_btc();
    auto wire = partly_filled_order();
    wire.state = WireOrderState::Canceled;
    wire.cancel_source = "14"; // IOC remainder

    const auto fill = map_order_fill(wire, table, k_ingest);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->qty.raw, 200);
    EXPECT_EQ(fill->leaves_qty.raw, 300);

    const auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    // expired: the remainder ran out and nobody cancelled it
    EXPECT_EQ(event->type, OrderEventType::Expired);
    EXPECT_EQ(event->leaves_qty.raw, 300);
    EXPECT_EQ(event->ts_event.ns, 1'786'392'410'702ULL * 1'000'000ULL);
}

TEST(OkxMap, AVenueCancelIsACancelAndAnAmendRefusalIsNeither) {
    const auto table = table_with_btc();
    auto wire = partly_filled_order();
    wire.trade_id = {};
    wire.state = WireOrderState::Canceled;
    wire.cancel_source = "31"; // the post-only order would have taken

    auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Canceled);

    wire.state = WireOrderState::MmpCanceled;
    wire.cancel_source = "39";
    event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Canceled);

    // amendResult -1 leaves the order working at its old price
    wire.state = WireOrderState::Live;
    wire.cancel_source = {};
    wire.amend_result = "-1";
    event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::ModifyRejected);

    wire.amend_result = {};
    wire.code = k_insufficient_balance;
    event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Rejected);

    wire.code = k_ok;
    event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Accepted);
    EXPECT_EQ(event->price.raw, 640'435);
    EXPECT_EQ(event->qty.raw, 500);
    EXPECT_EQ(event->leaves_qty.raw, 300);
}

TEST(OkxMap, QuantitiesOffTheLotAreRefusedRatherThanRounded) {
    const auto table = table_with_btc();
    auto wire = partly_filled_order();
    wire.fill_sz = "2.005"; // half a lot past a 0.01 step
    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());

    // a cumulative fill above the order size is dropped, not read as zero leaves
    wire = partly_filled_order();
    wire.acc_fill_sz = "9";
    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());

    wire = partly_filled_order();
    wire.sz = "5.005"; // the venue changed a lot size under us
    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());

    // an exec id that does not fit is no dedupe key
    wire = partly_filled_order();
    wire.trade_id = std::string_view{"744876981744876981744876981744876981744876981744876981744876981744876981"};
    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());
}

TEST(OkxMap, OnlyWorkingOrdersBecomeStatusReports) {
    const auto table = table_with_btc();
    auto wire = partly_filled_order();
    wire.state = WireOrderState::Live;
    wire.acc_fill_sz = "0";
    auto report = map_open_order(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Accepted);
    EXPECT_EQ(report->qty.raw, 500);
    EXPECT_EQ(report->leaves.raw, 500);
    EXPECT_EQ(report->exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(report->exchange_order_id.view(), "680800019749904385");

    wire.state = WireOrderState::PartiallyFilled;
    wire.acc_fill_sz = "2";
    report = map_open_order(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->leaves.raw, 300);

    for (const auto state :
         {WireOrderState::Filled, WireOrderState::Canceled, WireOrderState::MmpCanceled, WireOrderState::Unknown}) {
        wire.state = state;
        EXPECT_FALSE(map_open_order(wire, table).has_value());
    }
}

TEST(OkxMap, PositionSignComesFromPosSideAsWellAsFromPos) {
    const auto table = table_with_btc();
    WirePosition wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.pos = "12";
    wire.pos_side = "net";
    wire.avg_px = "64043.5";
    wire.u_ms = 1'786'392'410'702ULL;

    auto report = map_position(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, 1'200); // 12 contracts / 0.01
    EXPECT_EQ(report->avg_entry.raw, 640'435);

    // net mode signs pos itself
    wire.pos = "-12";
    report = map_position(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, -1'200);

    // long/short mode returns one unsigned row a side
    wire.pos = "12";
    wire.pos_side = "short";
    report = map_position(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, -1'200);

    wire.pos_side = "long";
    report = map_position(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, 1'200);
}

TEST(OkxMap, RestFillsBecomeFillReportsWithTheSameFeeConvention) {
    const auto table = table_with_btc();
    WireFill wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.trade_id = "744876980";
    wire.ord_id = "680800019749904384";
    wire.cl_ord_id = k_ulid;
    wire.side = "sell";
    wire.fill_px = "64043.5";
    wire.fill_sz = "2";
    wire.fee = "-0.0384261";
    wire.exec_type = "M";
    wire.fill_ms = 1'786'392'410'701ULL;

    const auto report = map_fill(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->exec_id.view(), "744876980");
    EXPECT_EQ(report->side, Side::Sell);
    EXPECT_EQ(report->price.raw, 640'435);
    EXPECT_EQ(report->qty.raw, 200);
    EXPECT_EQ(report->fee.raw, 38'426'100);
    EXPECT_EQ(report->liquidity, Liquidity::Maker);
    EXPECT_EQ(report->ts_venue.ns, 1'786'392'410'701ULL * 1'000'000ULL);

    // unstated liquidity stays Unknown
    wire.exec_type = {};
    EXPECT_EQ(map_fill(wire, table)->liquidity, Liquidity::Unknown);
}

TEST(OkxMap, PrivateFramesForInstrumentsWeNeverLoadedAreDropped) {
    const auto table = table_with_btc();
    auto wire = partly_filled_order();
    wire.inst_id = "DOGE-USDT-SWAP";
    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());
    EXPECT_FALSE(map_order_event(wire, table, k_ingest).has_value());
    EXPECT_FALSE(map_open_order(wire, table).has_value());
}

TEST(OkxErrors, AuthCodesHaltTheVenue) {
    for (const auto code : {k_invalid_api_key,
                            k_invalid_signature,
                            k_passphrase_incorrect,
                            k_ip_not_whitelisted,
                            k_ws_invalid_signature}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::AuthFailure) << code;
    }
}

TEST(OkxErrors, ClockSkewIsRetryableNotAuthFailure) {
    // clock errors are retryable: resync and resend
    for (const auto code : {k_timestamp_expired, k_invalid_timestamp, k_ws_timestamp_expired}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::Retryable) << code;
    }
}

TEST(OkxErrors, LapsedSessionIsRetryable) {
    EXPECT_EQ(classify(k_ws_login_required), exec::ErrorClass::Retryable);
}

TEST(OkxErrors, RateLimitCodesBackOff) {
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_ws_too_frequent), exec::ErrorClass::RateLimited);
}

TEST(OkxErrors, UnknownCodeIsTerminalByDefault) {
    // unknown codes are terminal
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_instrument_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_ws_wrong_channel), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_post_only_would_take), exec::ErrorClass::Terminal);
}

TEST(OkxErrors, BenignCancelRacesAreRecognised) {
    EXPECT_TRUE(is_benign_cancel_failure(k_cancel_order_not_exist));
    EXPECT_TRUE(is_benign_cancel_failure(k_cancel_already_canceled));
    EXPECT_TRUE(is_benign_cancel_failure(k_cancel_already_completed));
    EXPECT_TRUE(is_benign_cancel_failure(k_order_not_found));
    EXPECT_FALSE(is_benign_cancel_failure(k_insufficient_balance));

    // an amend of a settled order is benign
    EXPECT_TRUE(is_benign_amend_failure(k_amend_already_settled));
    EXPECT_TRUE(is_benign_amend_failure(k_cancel_order_not_exist));
    EXPECT_FALSE(is_benign_amend_failure(k_insufficient_balance));
}

TEST(OkxErrors, OrderEntryRefusalsAreTerminalNotRetried) {
    // these are final answers
    for (const auto code :
         {k_duplicate_param, k_tier_too_low_for_channel, k_post_only_would_take, k_operation_failed}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::Terminal) << code;
    }
}

TEST(OkxMapFunding, TheMarkArrivesOnItsOwnChannelAndJoinsTheSameRecord) {
    // rate and mark arrive on separate channels and must merge
    const auto table = table_with_btc();
    FundingMemo memo{};

    WireFundingRate rate{};
    rate.inst_id = "BTC-USDT-SWAP";
    rate.funding_rate = "0.0001";
    rate.funding_ms = 1'786'406'400'000ULL;
    const auto first = map_funding(rate, table, memo, k_ingest);
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->mark_price.is_undef()) << "no mark has been stated yet";

    WireMarkPrice mark{};
    mark.inst_id = "BTC-USDT-SWAP";
    mark.mark_price = "64163.2";
    const auto merged = map_mark_price(mark, table, memo, k_ingest);
    ASSERT_TRUE(merged.has_value());
    EXPECT_FALSE(merged->mark_price.is_undef());
    EXPECT_EQ(merged->rate.raw, 100'000) << "the rate the other channel stated is carried forward";
}

TEST(OkxMapFunding, ARateFrameDoesNotEraseAMarkAlreadyStated) {
    const auto table = table_with_btc();
    FundingMemo memo{};

    WireMarkPrice mark{};
    mark.inst_id = "BTC-USDT-SWAP";
    mark.mark_price = "64163.2";
    ASSERT_TRUE(map_mark_price(mark, table, memo, k_ingest).has_value());
    const auto held = memo.mark;

    WireFundingRate rate{};
    rate.inst_id = "BTC-USDT-SWAP";
    rate.funding_rate = "0.0001";
    const auto after = map_funding(rate, table, memo, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->mark_price.raw, held.raw) << "the mark survives a frame that omitted it";
}

TEST(OkxMap, BalanceTakesTheCashTotalAndNotTheEquity) {
    const auto body = fixture("rest_balance.json");
    auto storage = std::make_unique<BalanceBuffer>();
    const auto rows = parse_balances(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);

    const auto report = map_balance((*rows)[0], AccountMode::Futures, k_ingest);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->asset.view(), "USDT");
    // cashBal, not eq, which adds open pnl
    EXPECT_EQ(report->total.raw, 4'850'435'693'622);
    EXPECT_EQ(report->available.raw, 4'834'317'093'622);
    EXPECT_EQ(report->unrealized.raw, -7'545'600'000);
    EXPECT_EQ(report->ts_venue.ns, 1'705'449'605'015'000'000ULL);
}

TEST(OkxMap, BalanceWithholdsAvailableWhereTheAccountMarginsAcrossCurrencies) {
    const auto body = fixture("rest_balance.json");
    auto storage = std::make_unique<BalanceBuffer>();
    const auto rows = parse_balances(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_FALSE(rows->empty());
    const auto& wire = (*rows)[0];
    // spot and futures modes margin each currency out of itself
    EXPECT_FALSE(map_balance(wire, AccountMode::Spot, k_ingest)->available.is_undef());
    EXPECT_FALSE(map_balance(wire, AccountMode::Futures, k_ingest)->available.is_undef());
    // margin modes check account-wide equity, so availBal is not trusted
    EXPECT_TRUE(map_balance(wire, AccountMode::MultiCurrency, k_ingest)->available.is_undef());
    EXPECT_TRUE(map_balance(wire, AccountMode::Portfolio, k_ingest)->available.is_undef());
    // a failed config read is not a mode
    EXPECT_TRUE(map_balance(wire, AccountMode::Unknown, k_ingest)->available.is_undef());
    // total and pnl are published in every mode
    const auto pooled = map_balance(wire, AccountMode::Portfolio, k_ingest);
    EXPECT_EQ(pooled->total.raw, 4'850'435'693'622);
    EXPECT_EQ(pooled->unrealized.raw, -7'545'600'000);
}

TEST(OkxMap, BalanceLeavesAFieldOkxDidNotSendUndef) {
    WireCurrencyBalance wire{};
    wire.ccy = "BTC";
    wire.cash_bal = "0.25";
    const auto report = map_balance(wire, AccountMode::Spot, k_ingest);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->total.raw, 250'000'000);
    // an empty string is undef, not zero
    EXPECT_TRUE(report->available.is_undef());
    EXPECT_TRUE(report->unrealized.is_undef());
    // no uTime, so the caller's time is used
    EXPECT_EQ(report->ts_venue.ns, k_ingest.ns);
}

TEST(OkxMap, BalanceWithoutACurrencyIsDropped) {
    // a balance without a currency is refused
    WireCurrencyBalance wire{};
    wire.cash_bal = "12.5";
    EXPECT_FALSE(map_balance(wire, AccountMode::Spot, k_ingest).has_value());
    wire.ccy = "TWENTYONECHARACTERSXX";
    EXPECT_FALSE(map_balance(wire, AccountMode::Spot, k_ingest).has_value());
}

InstrumentBase classified_swap(std::string_view inst_id, std::string_view inst_category) {
    WireInstrument row{};
    row.inst_id = inst_id;
    row.inst_type = "SWAP";
    row.state = "live";
    row.tick_size = "0.01";
    row.lot_size = "1";
    row.min_size = "1";
    row.ct_val = "0.01";
    row.ct_mult = "1";
    row.ct_type = "linear";
    row.inst_category = inst_category;
    const auto mapped = map_instrument(row, MarketType::Perp);
    EXPECT_TRUE(mapped.has_value());
    auto base = mapped.has_value() ? base_of(*mapped) : InstrumentBase{};
    classify(VenueId::Okx, base);
    return base;
}

TEST(OkxMap, InstCategoryClassifiesTheUnderlying) {
    const auto tsla = classified_swap("TSLA-USDT-SWAP", "3");
    EXPECT_EQ(tsla.asset_class, AssetClass::Equity);
    EXPECT_EQ(tsla.underlying.view(), "TSLA");
    const auto xau = classified_swap("XAU-USDT-SWAP", "4");
    EXPECT_EQ(xau.asset_class, AssetClass::Commodity);
    EXPECT_EQ(xau.underlying.view(), "XAU");
    const auto spx = classified_swap("SPX-USDT-SWAP", "1");
    EXPECT_EQ(spx.asset_class, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");
    EXPECT_EQ(classified_swap("BND-USDT-SWAP", "6").asset_class, AssetClass::Unknown);
}

} // namespace
} // namespace raikiri::venues::okx
