#include <raikiri/net/inflater.hpp>
#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/book.hpp>
#include <raikiri/venues/bingx/catalogue.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/market_data.hpp>
#include <raikiri/venues/bingx/orders.hpp>
#include <raikiri/venues/bingx/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::bingx {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bingx) {
    hammer("bingx",
           {
               {"ws_*book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_ticker(f);
                }},
               {"ws_*trade.json",
                [](std::string_view f) {
                    static TradeBuffer storage;
                    (void)parse_event(f);
                    (void)parse_trades(f, storage);
                }},
               {"ws_depth100.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_event(f);
                    (void)parse_depth(f, storage, MarketType::Perp);
                }},
               {"ws_mark_price.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_mark_price(f);
                }},
               {"ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_spot_depth20.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_event(f);
                    (void)parse_depth(f, storage, MarketType::Spot);
                }},
               {"ws_subscribe_ack.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_control(f);
                }},
               {"ws_order_update_*",
                [](std::string_view f) {
                    (void)parse_private_event(f);
                    (void)parse_order_update(f);
                }},

               {"ws_listen_key_expired.json", [](std::string_view f) { (void)parse_private_event(f); }},
               {"rest_spot_ticker_24h.json",
                [](std::string_view f) {
                    static SpotTickerBuffer storage;
                    (void)parse_spot_tickers(f, storage);
                }},
               {"ws_spot_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_error*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_control(f);
                }},
               {"*.frame.gz",
                [](std::string_view f) {
                    static net::Inflater inflater;
                    static LevelBuffer storage;
                    if (const auto text = inflater.inflate(f); text.has_value()) {
                        (void)parse_event(*text);
                        (void)parse_control(*text);
                        (void)parse_depth(*text, storage, MarketType::Perp);
                    }
                }},
               {"rest_balance.json",
                [](std::string_view f) {
                    static BalanceBuffer storage;
                    (void)parse_balances(f, storage);
                }},
               {"rest_income.json",
                [](std::string_view f) {
                    static IncomeBuffer storage;
                    (void)parse_income(f, storage);
                }},
               {"rest_listen_key*", [](std::string_view f) { (void)parse_listen_key(f); }},
               {"rest_position_mode.json", [](std::string_view f) { (void)parse_position_mode(f); }},
               {"rest_server_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_open_orders_*",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_orders(f, storage);
                }},
               {"rest_contracts.json",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_contracts(f, storage);
                }},
               {"rest_spot_symbols.json",
                [](std::string_view f) {
                    static SpotSymbolBuffer storage;
                    (void)parse_spot_symbols(f, storage);
                }},
               {"rest_premium_index.json", [](std::string_view f) { (void)parse_premium_index(f); }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_order_ack_*", [](std::string_view f) { (void)parse_order_ack(f); }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::bingx
