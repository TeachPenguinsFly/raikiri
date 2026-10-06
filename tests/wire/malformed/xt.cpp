#include <raikiri/venues/xt/account.hpp>
#include <raikiri/venues/xt/book.hpp>
#include <raikiri/venues/xt/catalogue.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/market_data.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/venues/xt/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::xt {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Xt) {
    hammer("xt",
           {
               {"ws_depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels, MarketType::Perp);
                }},
               {"ws_depth_update*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth_update(f, levels);
                }},
               {"ws_spot_depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_depth(f, levels, MarketType::Spot);
                }},
               {"ws_trade_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_trade(f, MarketType::Perp);
                }},
               {"ws_spot_public_trade.json", [](std::string_view f) { (void)parse_trade(f, MarketType::Spot); }},
               {"ws_index_price.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_mark_price(f);
                }},
               {"ws_mark_price.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_mark_price(f);
                }},
               {"ws_*ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f, MarketType::Perp);
                    (void)parse_ticker(f, MarketType::Spot);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error_unknown_topic.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_ack(f);
                }},
               {"ws_private_*", [](std::string_view f) { (void)parse_private_frame(f); }},
               {"ws_futures_order_*",
                [](std::string_view f) {
                    (void)parse_private_frame(f);
                    (void)parse_futures_order(f);
                }},
               {"ws_futures_trade.json",
                [](std::string_view f) {
                    (void)parse_private_frame(f);
                    (void)parse_futures_deal(f);
                }},
               {"ws_spot_order_*",
                [](std::string_view f) {
                    (void)parse_private_frame(f);
                    (void)parse_spot_order_push(f);
                }},
               {"ws_spot_trade.json",
                [](std::string_view f) {
                    (void)parse_private_frame(f);
                    (void)parse_spot_trade_push(f);
                }},
               {"rest_symbol_list*",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_rest_envelope(f);
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_depth.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_funding_rate*",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_funding_rate(f);
                }},
               {"rest_server_time.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_server_time(f);
                }},
               {"rest_listen_key.json", [](std::string_view f) { (void)parse_listen_key(f); }},
               {"rest_ws_token.json", [](std::string_view f) { (void)parse_spot_ws_token(f); }},
               {"rest_futures_*", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_futures_balance_list.json",
                [](std::string_view f) {
                    static FuturesBalanceBuffer balances;
                    (void)parse_futures_balances(f, balances);
                }},
               {"rest_futures_bills.json",
                [](std::string_view f) {
                    static BillBuffer bills;
                    (void)parse_bills(f, bills);
                }},
               {"rest_futures_open_orders.json",
                [](std::string_view f) {
                    static FuturesOrderBuffer orders;
                    (void)parse_futures_orders(f, orders);
                }},
               {"rest_futures_order_list.json",
                [](std::string_view f) {
                    static FuturesOrderBuffer orders;
                    (void)parse_futures_orders(f, orders);
                }},
               {"rest_futures_trade_list.json",
                [](std::string_view f) {
                    static FuturesDealBuffer deals;
                    (void)parse_futures_deals(f, deals);
                }},
               {"rest_futures_positions.json",
                [](std::string_view f) {
                    static FuturesPositionBuffer positions;
                    (void)parse_futures_positions(f, positions);
                }},
               {"rest_*_ack.json", [](std::string_view f) { (void)parse_order_ack(f); }},
               {"rest_spot_*", [](std::string_view f) { (void)parse_spot_envelope(f); }},
               {"rest_spot_server_time.json", [](std::string_view f) { (void)parse_spot_server_time(f); }},
               {"rest_spot_symbol_list.json",
                [](std::string_view f) {
                    static SpotSymbolBuffer symbols;
                    (void)parse_spot_symbols(f, symbols);
                }},
               {"rest_spot_open_orders.json",
                [](std::string_view f) {
                    static SpotOrderBuffer orders;
                    (void)parse_spot_orders(f, orders);
                }},
               {"rest_spot_trades.json",
                [](std::string_view f) {
                    static SpotTradeBuffer trades;
                    (void)parse_spot_trades(f, trades);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::xt
