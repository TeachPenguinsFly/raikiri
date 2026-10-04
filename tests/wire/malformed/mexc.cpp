#include <raikiri/venues/mexc/account.hpp>
#include <raikiri/venues/mexc/book.hpp>
#include <raikiri/venues/mexc/catalogue.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/market_data.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::mexc {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Mexc) {
    hammer("mexc",
           {
               {"ws_ticker*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_deal.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_depth_full*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    static LevelBuffer levels_two;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                    (void)parse_event_and_depth(f, levels_two);
                }},
               {"ws_depth_update*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_login_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_idle_timeout.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_personal_order_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_futures_order(f);
                }},
               {"ws_personal_deal.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_futures_deal(f);
                }},
               {"ws_personal_position.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_futures_position(f);
                }},
               {"ws_spot_book_ticker.pb",
                [](std::string_view f) {
                    const auto event = parse_stream_event(f);
                    (void)parse_stream_quote(event ? event->payload : f);
                }},
               {"ws_spot_deals.pb",
                [](std::string_view f) {
                    static StreamTradeBuffer trades;
                    const auto event = parse_stream_event(f);
                    (void)parse_stream_trades(event ? event->payload : f, trades);
                }},
               {"ws_spot_depth.pb",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    const auto event = parse_stream_event(f);
                    (void)parse_stream_book(event ? event->payload : f, levels);
                }},
               {"rest_contract_detail*",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_exchange_info.json",
                [](std::string_view f) {
                    static SpotSymbolBuffer symbols;
                    (void)parse_spot_symbols(f, symbols);
                }},
               {"rest_depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_depth(f, levels);
                }},
               {"rest_ticker.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_ticker(f);
                }},
               {"rest_spot_ticker_24h.json",
                [](std::string_view f) {
                    static SpotTickerBuffer tickers;
                    (void)parse_spot_tickers(f, tickers);
                }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_futures_cancel_partial.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_spot_error_*", [](std::string_view f) { (void)parse_spot_error(f); }},
               {"rest_server_time.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_server_time(f);
                }},
               {"rest_funding_rate.json", [](std::string_view f) { (void)parse_funding_rate(f); }},
               {"rest_funding_rates.json",
                [](std::string_view f) {
                    static FundingRateBuffer rates;
                    (void)parse_funding_rates(f, rates);
                }},
               {"rest_funding_records.json",
                [](std::string_view f) {
                    static FundingRecordBuffer records;
                    (void)parse_funding_records(f, records);
                }},
               {"rest_futures_open_orders.json",
                [](std::string_view f) {
                    static FuturesOrderBuffer orders;
                    (void)parse_futures_orders(f, orders);
                }},
               {"rest_futures_order_deals.json",
                [](std::string_view f) {
                    static FuturesDealBuffer deals;
                    (void)parse_futures_deals(f, deals);
                }},
               {"rest_futures_positions.json",
                [](std::string_view f) {
                    static FuturesPositionBuffer positions;
                    (void)parse_futures_positions(f, positions);
                }},
               {"rest_spot_open_orders.json",
                [](std::string_view f) {
                    static SpotOrderBuffer orders;
                    (void)parse_spot_orders(f, orders);
                }},
               {"rest_spot_order_new.json", [](std::string_view f) { (void)parse_spot_order(f); }},
               {"rest_spot_my_trades.json",
                [](std::string_view f) {
                    static SpotTradeBuffer trades;
                    (void)parse_spot_trades(f, trades);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::mexc
