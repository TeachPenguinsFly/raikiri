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
               {"ws_book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_ticker(f);
                }},
               {"ws_trade.json",
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
               {"ws_order_update_perp_fill.json",
                [](std::string_view f) {
                    (void)parse_private_event(f);
                    (void)parse_order_update(f);
                }},
               {"ws_order_update_spot_fill.json",
                [](std::string_view f) {
                    (void)parse_private_event(f);
                    (void)parse_order_update(f);
                }},
               {"ws_listen_key_expired.json", [](std::string_view f) { (void)parse_private_event(f); }},
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
               {"rest_error_bad_symbol.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_order_ack_perp.json", [](std::string_view f) { (void)parse_order_ack(f); }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::bingx
