#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/catalogue.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/market_data.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::bitfinex {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bitfinex) {
    hammer("bitfinex",
           {
               {"ws_heartbeat.json", [](std::string_view f) { (void)parse_frame(f); }},
               {"ws_subscribed_book.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_subscribed(f);
                }},
               {"ws_error_symbol.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_auth_ok.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_auth_ack(f);
                }},
               {"ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_ticker(f);
                }},
               {"ws_trade_te.json",
                [](std::string_view f) {
                    static TradeBuffer storage;
                    (void)parse_frame(f);
                    (void)parse_trades(f, storage);
                }},
               {"ws_book_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_frame(f);
                    (void)parse_book(f, storage);
                }},
               {"ws_book_update.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_frame(f);
                    (void)parse_book(f, storage);
                }},
               {"ws_deriv_status.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_deriv_status(f);
                }},
               {"ws_order_new.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_order(f);
                }},
               {"ws_orders_snapshot.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_frame(f);
                    (void)parse_orders(f, storage);
                }},
               {"ws_exec_te.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_execution(f);
                }},
               {"ws_positions_snapshot.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_frame(f);
                    (void)parse_positions(f, storage);
                }},
               {"rest_trades.json",
                [](std::string_view f) {
                    static ExecutionBuffer storage;
                    (void)parse_executions(f, storage);
                }},
               {"rest_conf_pair.json",
                [](std::string_view f) {
                    static PairBuffer storage;
                    (void)parse_pair_info(f, storage);
                }},
               {"rest_platform_status.json",
                [](std::string_view f) { (void)parse_platform_status(f); }},
               {"rest_error.json",
                [](std::string_view f) {
                    (void)is_rest_error(f);
                    (void)parse_rest_error(f);
                }},
               {"rest_order_submit.json", [](std::string_view f) { (void)parse_notification(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::bitfinex
