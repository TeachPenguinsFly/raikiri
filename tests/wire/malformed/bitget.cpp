#include <raikiri/venues/bitget/account.hpp>
#include <raikiri/venues/bitget/book.hpp>
#include <raikiri/venues/bitget/catalogue.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/market_data.hpp>
#include <raikiri/venues/bitget/orders.hpp>
#include <raikiri/venues/bitget/positions.hpp>
#include <raikiri/venues/bitget/trade.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::bitget {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bitget) {
    hammer("bitget",
           {
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error_unknown_instrument.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_login_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_trade_place_order.json", [](std::string_view f) { (void)parse_trade_reply(f); }},
               {"ws_trade_error.json", [](std::string_view f) { (void)parse_trade_reply(f); }},
               {"ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_trade.json",
                [](std::string_view f) {
                    static TradeBuffer storage;
                    (void)parse_event(f);
                    (void)parse_trades(f, storage);
                }},
               {"ws_books15.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_event(f);
                    (void)parse_books(f, storage);
                }},
               {"ws_books_update.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_event(f);
                    (void)parse_books(f, storage);
                }},
               {"ws_orders_mix_accepted.json",
                [](std::string_view f) {
                    static OrderUpdateBuffer storage;
                    (void)parse_event(f);
                    (void)parse_order_updates(f, storage);
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
               {"rest_orderbook.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_rest_orderbook(f, storage);
                }},
               {"rest_current_fund_rate.json", [](std::string_view f) { (void)parse_current_fund_rate(f); }},
               {"rest_error_unknown_symbol.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_mix_account_bill.json",
                [](std::string_view f) {
                    static BillBuffer storage;
                    (void)parse_mix_bills(f, storage);
                }},
               {"rest_mix_orders_pending.json",
                [](std::string_view f) {
                    static OrderUpdateBuffer storage;
                    (void)parse_mix_orders(f, storage);
                }},
               {"rest_mix_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
               {"rest_order_ack.json", [](std::string_view f) { (void)parse_order_ack(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::bitget
