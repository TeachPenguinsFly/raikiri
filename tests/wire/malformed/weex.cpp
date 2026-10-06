#include <raikiri/venues/weex/account.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/positions.hpp>
#include <raikiri/venues/weex/spot.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::weex {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Weex) {
    hammer("weex",
           {
               {"ws_depth*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_trade_*",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_event(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_subscribe_refused.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_ping.json", [](std::string_view f) { (void)parse_event(f); }},
               {"spot_ws_book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_ticker(f);
                }},
               {"spot_ws_trade.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_event(f);
                    (void)parse_trades(f, prints);
                }},
               {"spot_ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"spot_rest_exchange_info.json",
                [](std::string_view f) {
                    static SpotInstrumentBuffer instruments;
                    (void)parse_spot_exchange_info(f, instruments);
                }},
               {"spot_rest_ticker_24h.json",
                [](std::string_view f) {
                    static SpotTickerBuffer rows;
                    (void)parse_spot_tickers(f, rows);
                }},
               {"spot_rest_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"spot_rest_error_*", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"spot_rest_*_ack.json",
                [](std::string_view f) {
                    (void)parse_rest_error(f);
                    (void)parse_spot_ack(f);
                    (void)spot_verdict(false, 200, f);
                }},
               {"spot_rest_open_orders.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
               {"spot_rest_order.json", [](std::string_view f) { (void)parse_order(f); }},
               {"spot_rest_my_trades.json",
                [](std::string_view f) {
                    static SpotTradeBuffer trades;
                    (void)parse_spot_trades(f, trades);
                }},
               {"spot_rest_account.json",
                [](std::string_view f) {
                    static SpotBalanceBuffer balances;
                    (void)parse_spot_account(f, balances);
                }},
               {"spot_rest_api_trading_symbols.json",
                [](std::string_view f) {
                    static TradableBuffer symbols;
                    (void)parse_api_trading_symbols(f, symbols);
                }},
               {"spot_ws_private_orders*",
                [](std::string_view f) {
                    static SpotOrderPushBuffer orders;
                    (void)parse_spot_private(f);
                    (void)parse_spot_order_push(f, orders);
                }},
               {"spot_ws_private_fill.json",
                [](std::string_view f) {
                    static SpotFillPushBuffer fills;
                    (void)parse_spot_private(f);
                    (void)parse_spot_fill_push(f, fills);
                }},
               {"spot_ws_private_account.json", [](std::string_view f) { (void)parse_spot_private(f); }},
               {"spot_ws_private_ping.json", [](std::string_view f) { (void)parse_spot_private(f); }},
               {"spot_ws_private_ack.json", [](std::string_view f) { (void)parse_spot_private(f); }},
               {"ws_connected.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_exchange_info.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_rest_error(f);
                    (void)parse_exchange_info(f, instruments);
                }},
               {"rest_api_trading_symbols.json",
                [](std::string_view f) {
                    static TradableBuffer symbols;
                    (void)parse_api_trading_symbols(f, symbols);
                }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"rest_premium_index.json",
                [](std::string_view f) {
                    static PremiumBuffer premiums;
                    (void)parse_premium_index(f, premiums);
                }},
               {"rest_open_orders.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
               {"rest_*_ack.json", [](std::string_view f) { (void)parse_order_ack(f); }},
               {"rest_cancel_batch.json",
                [](std::string_view f) {
                    static OrderAckBuffer acks;
                    (void)parse_order_acks(f, acks);
                }},
               {"rest_user_trades.json",
                [](std::string_view f) {
                    static FillBuffer fills;
                    (void)parse_fills(f, fills);
                }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer positions;
                    (void)parse_positions(f, positions);
                }},
               {"rest_balance.json",
                [](std::string_view f) {
                    static BalanceBuffer balances;
                    (void)parse_balances(f, balances);
                }},
               {"rest_income.json",
                [](std::string_view f) {
                    static BillBuffer bills;
                    (void)parse_bills(f, bills);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::weex
