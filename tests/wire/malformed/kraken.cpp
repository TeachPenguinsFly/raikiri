#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/catalogue.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/market_data.hpp>
#include <raikiri/venues/kraken/orders.hpp>
#include <raikiri/venues/kraken/positions.hpp>
#include <raikiri/venues/kraken/spot.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::kraken {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Kraken) {
    hammer("kraken",
           {
               {"ws_ticker_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_trade*",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_book_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_alert_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_alert(f);
                }},
               {"ws_subscribed.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_info.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_challenge.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_challenge(f);
                }},
               {"rest_ticker.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_send_order_*",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_order_status(f, StatusKind::Send);
                }},
               {"rest_cancel_order.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_order_status(f, StatusKind::Cancel);
                }},
               {"rest_edit_order.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_order_status(f, StatusKind::Edit);
                }},
               {"rest_instruments*",
                [](std::string_view f) {
                    static InstrumentBuffer storage;
                    (void)parse_instruments(f, storage);
                }},
               {"ws_open_orders_*",
                [](std::string_view f) {
                    static OrderSnapshotBuffer storage;
                    (void)parse_order_snapshot(f, storage);
                    (void)parse_order_delta(f);
                }},
               {"ws_fills_*",
                [](std::string_view f) {
                    static WsFillBuffer storage;
                    (void)parse_ws_fills(f, storage);
                }},
               {"rest_open_orders.json",
                [](std::string_view f) {
                    static OpenOrderBuffer storage;
                    (void)parse_open_orders(f, storage);
                }},
               {"rest_fills.json",
                [](std::string_view f) {
                    static FillBuffer storage;
                    (void)parse_fills(f, storage);
                }},
               {"rest_open_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
               {"spot_rest_asset_pairs.json",
                [](std::string_view f) {
                    static SpotPairBuffer storage;
                    (void)parse_asset_pairs(f, storage);
                }},
               {"spot_ws_*", [](std::string_view f) { (void)parse_spot_event(f); }},
               {"spot_ws_ticker_*", [](std::string_view f) { (void)parse_spot_ticker(f); }},
               {"spot_ws_trade*",
                [](std::string_view f) {
                    static SpotTradeBuffer storage;
                    (void)parse_spot_trades(f, storage);
                }},
               {"spot_ws_book_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_spot_book(f, levels);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::kraken
