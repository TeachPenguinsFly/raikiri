#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/market_data.hpp>
#include <raikiri/venues/kraken/orders.hpp>
#include <raikiri/venues/kraken/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::kraken {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Kraken) {
    hammer("kraken",
           {
               {"ws_ticker_pf.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_ticker_pi.json", [](std::string_view f) { (void)parse_ticker(f); }},
               {"ws_trade.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_trade_snapshot.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_trades(f, trades);
                }},
               {"ws_book_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_book_delta.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_book(f, levels);
                }},
               {"ws_alert_bad_product.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_alert(f);
                }},
               {"ws_subscribed.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_challenge.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_challenge(f);
                }},
               {"rest_ticker.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_error_auth.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"ws_open_orders_new.json", [](std::string_view f) { (void)parse_order_delta(f); }},
               {"ws_fills_snapshot.json",
                [](std::string_view f) {
                    static WsFillBuffer storage;
                    (void)parse_ws_fills(f, storage);
                }},
               {"rest_open_orders.json",
                [](std::string_view f) {
                    static OpenOrderBuffer storage;
                    (void)parse_open_orders(f, storage);
                }},
               {"rest_open_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::kraken
