#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/bybit/book.hpp>
#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/market_data.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/venues/bybit/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::bybit {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bybit) {
    hammer("bybit",
           {
               {"orderbook_snapshot.json",
                [](std::string_view f) {
                    static OrderbookBuffer ob;
                    (void)parse_topic(f);
                    (void)parse_orderbook(f, ob);
                }},
               {"orderbook_delta.json",
                [](std::string_view f) {
                    static OrderbookBuffer ob;
                    (void)parse_topic(f);
                    (void)parse_orderbook(f, ob);
                }},
               {"public_trade.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_topic(f);
                    (void)parse_trades(f, trades);
                }},
               {"tickers_snapshot.json",
                [](std::string_view f) {
                    (void)parse_topic(f);
                    (void)parse_ticker(f);
                }},
               {"tickers_delta.json",
                [](std::string_view f) {
                    (void)parse_topic(f);
                    (void)parse_ticker(f);
                }},
               {"order_update.json",
                [](std::string_view f) {
                    static OrderUpdateBuffer updates;
                    (void)parse_topic(f);
                    (void)parse_order_updates(f, updates);
                }},
               {"order_rejected.json",
                [](std::string_view f) {
                    static OrderUpdateBuffer updates;
                    (void)parse_topic(f);
                    (void)parse_order_updates(f, updates);
                }},
               {"execution.json",
                [](std::string_view f) {
                    static ExecutionBuffer execs;
                    (void)parse_topic(f);
                    (void)parse_executions(f, execs);
                }},
               {"position.json",
                [](std::string_view f) {
                    static PositionBuffer positions;
                    (void)parse_topic(f);
                    (void)parse_positions(f, positions);
                }},
               {"pong.json", [](std::string_view f) { (void)parse_topic(f); }},
               {"rest_error.json", [](std::string_view f) { (void)parse_envelope(f); }},
               {"rest_order_ack.json",
                [](std::string_view f) {
                    (void)parse_envelope(f);
                    (void)parse_order_ack(f);
                }},
               {"rest_open_orders.json",
                [](std::string_view f) {
                    static OrderUpdateBuffer orders;
                    (void)parse_envelope(f);
                    (void)parse_open_orders(f, orders);
                }},
               {"rest_instruments.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_wallet_balance.json",
                [](std::string_view f) {
                    static CoinBalanceBuffer coins;
                    (void)parse_envelope(f);
                    (void)parse_wallet_balance(f, coins);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::bybit
