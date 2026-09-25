#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::okx {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Okx) {
    hammer("okx",
           {
               {"ws_bbo_tbt.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_funding_rate.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_funding_rate(f);
                }},
               {"ws_tickers.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_books_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_books(f, levels);
                }},
               {"ws_books_update.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_books(f, levels);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_login_failed.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_orders_fill.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_event(f);
                    (void)parse_orders(f, orders);
                }},
               {"rest_instruments.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_rest_envelope(f);
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_error_bad_instrument.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_mark_price.json", [](std::string_view f) { (void)parse_mark_price(f); }},
               {"rest_fills.json",
                [](std::string_view f) {
                    static FillBuffer fills;
                    (void)parse_fills(f, fills);
                }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer positions;
                    (void)parse_positions(f, positions);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::okx
