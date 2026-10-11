#include <raikiri/venues/blofin/trade.hpp>
#include <raikiri/venues/blofin/book.hpp>
#include <raikiri/venues/blofin/catalogue.hpp>
#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/venues/blofin/market_data.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::blofin {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Blofin) {
    hammer("blofin",
           {
               {"ws_books_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_books(f, levels);
                }},
               {"ws_books_update*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_books(f, levels);
                }},

               {"ws_trades.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_event(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_tickers.json",
                [](std::string_view f) {
                    static TickerBuffer tickers;
                    (void)parse_event(f);
                    (void)parse_tickers(f, tickers);
                }},
               {"ws_tickers_inverse.json",
                [](std::string_view f) {
                    static TickerBuffer tickers;
                    (void)parse_event(f);
                    (void)parse_tickers(f, tickers);
                }},
               {"ws_funding_rate.json",
                [](std::string_view f) {
                    static FundingBuffer funding;
                    (void)parse_event(f);
                    (void)parse_funding(f, funding);
                }},
               {"ws_error_invalid_request.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_instruments.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_rest_envelope(f);
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_funding_rate.json",
                [](std::string_view f) {
                    static FundingBuffer funding;
                    (void)parse_rest_envelope(f);
                    (void)parse_funding(f, funding);
                }},
               {"rest_open_interest.json",
                [](std::string_view f) {
                    static OpenInterestBuffer rows;
                    (void)parse_rest_envelope(f);
                    (void)parse_open_interest(f, rows);
                }},
               {"rest_mark_price.json",
                [](std::string_view f) {
                    static MarkBuffer marks;
                    (void)parse_rest_envelope(f);
                    (void)parse_mark_table(f, marks);
                }},
               {"rest_place_order.json", [](std::string_view f) { (void)parse_reply(f); }},
               {"rest_cancel_order.json", [](std::string_view f) { (void)parse_reply(f); }},
               {"rest_error_no_such_key.json", [](std::string_view f) { (void)parse_reply(f); }},
               {"rest_orders_pending.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_orders(f, storage);
                }},
               {"rest_fills_history.json",
                [](std::string_view f) {
                    static FillBuffer storage;
                    (void)parse_fills(f, storage);
                }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
               {"rest_balance.json",
                [](std::string_view f) {
                    static BalanceBuffer storage;
                    (void)parse_balances(f, storage);
                }},
               {"ws_orders_snapshot.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_private(f, storage);
                }},
               {"ws_login.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_private(f, storage);
                }},
               {"ws_login_failed.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_private(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::blofin
