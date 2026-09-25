#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/catalogue.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::gate {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Gate) {
    hammer("gate",
           {
               {"ws_book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_ticker(f);
                }},
               {"ws_order_book_update.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book_update(f, levels);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades, MarketType::Perp);
                }},
               {"ws_tickers.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_order_book_seed.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_order_book(f, levels);
                }},
               {"rest_contracts.json",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_contracts(f, storage);
                }},
               {"rest_server_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_error_invalid_param.json", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"ws_futures_order_new.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_ws_orders(f, storage, MarketType::Perp);
                }},
               {"ws_futures_usertrade.json",
                [](std::string_view f) {
                    static UserTradeBuffer storage;
                    (void)parse_ws_user_trades(f, storage, MarketType::Perp);
                }},
               {"rest_futures_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::gate
