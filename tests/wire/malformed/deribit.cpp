#include <raikiri/venues/deribit/account.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/catalogue.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/market_data.hpp>
#include <raikiri/venues/deribit/orders.hpp>
#include <raikiri/venues/deribit/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::deribit {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Deribit) {
    hammer("deribit",
           {
               {"rest_transaction_log_*",
                [](std::string_view f) {
                    static LogBuffer storage;
                    (void)parse_transaction_log(f, storage);
                }},
               {"ws_book_bbo*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_book_snapshot*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_book_change*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_ticker_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_heartbeat_test_request.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_subscribe_ack*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_response(f);
                }},
               {"ws_error_raw_unauthorized.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_response(f);
                }},
               {"ws_user_changes_*",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    static UserTradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_user_changes(f, orders, trades);
                }},
               {"rest_get_instruments*",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_get_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_open_orders.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
               {"rest_order_state_*",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer positions;
                    (void)parse_positions(f, positions);
                }},
               {"rest_user_trades.json",
                [](std::string_view f) {
                    static UserTradeBuffer trades;
                    (void)parse_user_trades(f, trades);
                }},
               {"rest_private_buy*",
                [](std::string_view f) {
                    static UserTradeBuffer trades;
                    (void)parse_response(f);
                    (void)parse_order_ack(f, trades);
                }},
               {"rest_error_*", [](std::string_view f) { (void)parse_response(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::deribit
