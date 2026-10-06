#include <raikiri/venues/okx/account.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/positions.hpp>
#include <raikiri/venues/okx/trade.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::okx {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Okx) {
    hammer("okx",
           {
               {"ws_trade_*",
                [](std::string_view f) {
                    static AckBuffer acks;
                    (void)parse_trade_reply(f);
                    (void)parse_order_acks(f, acks);
                }},
               {"ws_bbo_tbt*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_*trades.json",
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
               {"ws_tickers*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"rest_open_interest.json",
                [](std::string_view f) {
                    static OpenInterestBuffer storage;
                    (void)parse_open_interest(f, storage);
                }},
               {"rest_tickers_spot.json",
                [](std::string_view f) {
                    static TickerBuffer storage;
                    (void)parse_tickers(f, storage);
                }},
               {"ws_index_tickers.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_index_ticker(f);
                }},
               {"ws_mark_price_option.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_mark_price(f);
                }},
               {"ws_opt_summary*",
                [](std::string_view f) {
                    static SummaryBuffer summaries;
                    (void)parse_event(f);
                    (void)parse_opt_summary(f, summaries);
                }},
               {"ws_books_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_books(f, levels);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_login_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_orders_*",
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
               {"rest_option_instruments.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_rest_envelope(f);
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_option_underlyings.json",
                [](std::string_view f) {
                    static UnderlyingBuffer underlyings;
                    (void)parse_underlyings(f, underlyings);
                }},
               {"rest_error_bad_instrument.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_server_time.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_server_time(f);
                }},
               {"rest_mark_price.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_mark_price(f);
                }},
               {"rest_funding_rate.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_funding_rate(f);
                }},
               {"rest_order_*",
                [](std::string_view f) {
                    static AckBuffer acks;
                    (void)parse_rest_envelope(f);
                    (void)parse_order_acks(f, acks);
                }},
               {"rest_orders_pending.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
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
               {"rest_balance.json",
                [](std::string_view f) {
                    static BalanceBuffer balances;
                    (void)parse_balances(f, balances);
                }},
               {"rest_account_config.json", [](std::string_view f) { (void)parse_account_mode(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::okx
