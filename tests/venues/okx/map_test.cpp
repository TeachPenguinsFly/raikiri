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
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

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
    auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 10'000'000); // 0.01 * 1e9

    // The inverse contract is worth 100 USD, and ctMult multiplies it.
    wire.inst_id = "BTC-USD-SWAP";
    wire.ct_val = "100";
    wire.ct_mult = "2";
    instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 200 * k_e9);

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
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
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
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.tick_size = "0.1";
    wire.lot_size = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.lot_size = "0.01";
    wire.ct_val = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
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
    EXPECT_FALSE(is_benign_cancel_failure(k_insufficient_balance));
}

} // namespace
} // namespace raikiri::venues::okx
