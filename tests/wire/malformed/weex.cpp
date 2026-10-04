#include <raikiri/venues/weex/account.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/positions.hpp>

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
