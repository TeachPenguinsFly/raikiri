#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>

#include <gtest/gtest.h>

namespace raikiri::venues::okx {
namespace {

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

// A 5-contract IOC on BTC-USDT-SWAP that has filled 2, as the `orders`
// channel writes it. Every number divides exactly by the 0.1 tick and the
// 0.01 lot.
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
    // A price that does not divide by the tick means OKX changed an
    // instrument's precision under us. Rounding would put an order one tick
    // from where it was meant.
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
    // A contract with no bid is a real state, and an empty side must not turn
    // into a zero price at the top of book.
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
    // OKX states the aggressor's own direction, so this mapper must NOT
    // invert. Copying binance's flip here would reverse every signed print.
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
    // Not 1970: a zero timestamp reaching a latency histogram poisons it.
    EXPECT_EQ(trade->ts_event.ns, k_ingest.ns);
}

TEST(OkxMap, FundingUsesFundingTimeAndDerivesTheInterval) {
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = "0.0001000000000000";
    wire.funding_ms = 1'786'406'400'000ULL;
    wire.next_funding_ms = 1'786'435'200'000ULL;
    wire.ts_ms = 1'786'392'377'160ULL;

    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 0.0001 * 1e9
    // fundingTime, not nextFundingTime: the latter is one settlement further
    // out and would put every countdown an interval into the future.
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'406'400'000ULL * 1'000'000ULL);
    // The gap between the two states the interval exactly; the catalogue does
    // not carry it and OKX does not run 8 h on every contract.
    EXPECT_EQ(funding->interval_s, 28'800U);
    // Mark arrives on its own channel here, so it stays undefined rather than
    // being published as a real price of zero.
    EXPECT_TRUE(funding->mark_price.is_undef());
}

TEST(OkxMap, ARateWithMorePrecisionThanWeCarryStillArrives) {
    // OKX pads the rate to sixteen fractional digits. Read with the exact
    // decimal parser — as this was, for as long as the fixture happened to
    // hold a round number — every live rate was refused and the funding
    // channel published nothing at all while looking subscribed.
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = "0.0000439269860429";
    wire.funding_ms = 1'786'406'400'000ULL;

    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // Exact through 1e-9, and the dust below it is dropped toward zero.
    EXPECT_EQ(funding->rate.raw, 43'926);
}

TEST(OkxMap, EmptyForecastIsNotAPredictionOfZero) {
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = "0.0001";
    wire.next_funding_rate = {}; // observed live: shipped empty, not omitted
    wire.funding_ms = 1'786'406'400'000ULL;
    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_TRUE(funding->predicted_rate.is_undef());
}

TEST(OkxMap, ContractWithoutFundingIsNotReportedAsZero) {
    const auto table = table_with_btc();
    WireFundingRate wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.funding_rate = {}; // absent, not "0"
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

TEST(OkxMap, ContractValueComesFromTheCatalogueNotFromOne) {
    // A SWAP quotes in contracts. Pinning contract_value to 1 the way binance
    // USD-M allows would misprice every BTC-USDT-SWAP notional by 100x.
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

    // The inverse contract is worth 100 USD, and ctMult multiplies it. Same
    // shape of number as above, opposite currency and opposite PnL formula —
    // ctType is the only thing that separates them.
    wire.inst_id = "BTC-USD-SWAP";
    wire.ct_val = "100";
    wire.ct_mult = "2";
    wire.ct_type = "inverse";
    instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 200 * k_e9);
    EXPECT_EQ(base_of(*instrument).kind, ContractKind::Inverse);

    // A fractional multiplier has no defined meaning, so the row is refused
    // rather than approximated.
    wire.ct_mult = "1.5";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(OkxMap, NonLiveAndWrongMarketRowsAreRefused) {
    // A preopen row ships every numeric field empty, so the state check has to
    // come before the steps are parsed or the row fails for the wrong reason.
    WireInstrument wire{};
    wire.inst_id = "JP225-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.state = "preopen";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    // Asking for perps and getting a dated future back is a position bug, and
    // both types share the instruments endpoint.
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
    // OKX ships ctType on every SWAP row. A row that does not is a row we
    // cannot price: ctVal 100 is 100 USD on an inverse contract and 100 BTC
    // on a linear one, and both parse. Refuse rather than assume.
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
    // Every later conversion divides by these, so a zero step must never
    // reach the table.
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

// ------------------------------------------------------- order events ----

TEST(OkxMap, AFillCarriesItsTradeAndTheFeeSignFlips) {
    const auto table = table_with_btc();
    const auto fill = map_order_fill(partly_filled_order(), table, k_ingest);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->side, Side::Buy);
    EXPECT_EQ(fill->liquidity, Liquidity::Taker);
    EXPECT_EQ(fill->price.raw, 640'435); // 64043.5 / 0.1
    EXPECT_EQ(fill->qty.raw, 200);       // 2 contracts / 0.01
    // OKX publishes no remainder: sz minus accFillSz is the only source.
    EXPECT_EQ(fill->leaves_qty.raw, 300);
    EXPECT_EQ(fill->exec_id.view(), "744876981");
    EXPECT_EQ(fill->exchange_client_order_id.view(), k_ulid);
    // Negative on the wire means a fee charged. Copying the sign through
    // would credit the ledger for every trade we paid for.
    EXPECT_EQ(fill->fee.raw, 38'426'100);
    EXPECT_EQ(fill->ts_event.ns, 1'786'392'410'701ULL * 1'000'000ULL);

    // A partial fill carries no lifecycle transition of its own.
    EXPECT_FALSE(map_order_event(partly_filled_order(), table, k_ingest).has_value());
}

TEST(OkxMap, AFilledStateWithoutATradeIsNotASecondFill) {
    // OKX closes a spot market order with state "filled" and fillSz 0 after
    // the quantity already arrived on tradeId-bearing pushes. Reading
    // accFillSz off this one books the same fill twice.
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
    // The race the brief exists for: an IOC that filled part of itself and was
    // killed arrives as ONE object. Both halves have to come out, fill first.
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
    // Expired, not Canceled: nobody asked for this, the order ran out of book.
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

    // amendResult "-1" leaves the order working at its old price. A Rejected
    // here would close an order the venue still holds.
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

    // A cumulative fill larger than the order it belongs to is not a
    // remainder of zero. A fabricated zero closes the order at the wrong
    // quantity; dropping the event leaves a live order for the pulls to fix.
    wire = partly_filled_order();
    wire.acc_fill_sz = "9";
    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());

    wire = partly_filled_order();
    wire.sz = "5.005"; // the venue changed a lot size under us
    EXPECT_FALSE(map_order_fill(wire, table, k_ingest).has_value());

    // An exec id we cannot store is a dedupe key we do not have.
    wire = partly_filled_order();
    wire.trade_id = std::string_view{
        "744876981744876981744876981744876981744876981744876981744876981744876981"};
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

    for (const auto state : {WireOrderState::Filled,
                             WireOrderState::Canceled,
                             WireOrderState::MmpCanceled,
                             WireOrderState::Unknown}) {
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

    // Net mode signs `pos` itself.
    wire.pos = "-12";
    report = map_position(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, -1'200);

    // Long/short mode returns one UNSIGNED row a side, and reading a short as
    // written is a position error of twice the size.
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

    // Anything the venue never said stays Unknown rather than becoming Taker.
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
    // The request was fine, our clock was not. Resync and resend succeeds, and
    // filing these under AuthFailure would halt a venue whose keys are valid.
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
    // Deliberate: an unknown refusal retried at full speed earns an IP ban,
    // while a spuriously terminal order is recovered by reconciliation.
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

    // An amend of an order that already settled is the same race seen from
    // the other side; the private stream has already reported the outcome.
    EXPECT_TRUE(is_benign_amend_failure(k_amend_already_settled));
    EXPECT_TRUE(is_benign_amend_failure(k_cancel_order_not_exist));
    EXPECT_FALSE(is_benign_amend_failure(k_insufficient_balance));
}

TEST(OkxErrors, OrderEntryRefusalsAreTerminalNotRetried) {
    // A duplicate client order id, a tier too low for a channel and a
    // post-only that would have crossed are all final answers. Retrying any
    // of them at full speed is how an account earns a rate-limit ban.
    for (const auto code : {k_duplicate_param,
                            k_tier_too_low_for_channel,
                            k_post_only_would_take,
                            k_operation_failed}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::Terminal) << code;
    }
}

} // namespace
} // namespace raikiri::venues::okx
