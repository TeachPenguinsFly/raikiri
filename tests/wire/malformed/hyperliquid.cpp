#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::hyperliquid {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Hyperliquid) {
    hammer("hyperliquid",
           {
               {"ws_bbo.json",
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
               {"ws_l2book_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_l2book_next.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_book(f, levels);
                }},
               {"ws_active_asset_ctx.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_asset_ctx(f);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_meta.json",
                [](std::string_view f) {
                    static AssetBuffer storage;
                    (void)parse_meta(f, storage);
                }},
               {"rest_meta_and_asset_ctxs.json",
                [](std::string_view f) {
                    static AssetCtxBuffer storage;
                    (void)parse_asset_ctxs(f, storage);
                }},
               {"exchange_order_resting.json",
                [](std::string_view f) {
                    static ExchangeStatusBuffer storage;
                    (void)parse_exchange_reply(f, storage);
                }},
               {"exchange_order_error.json",
                [](std::string_view f) {
                    static ExchangeStatusBuffer storage;
                    (void)parse_exchange_reply(f, storage);
                }},
               {"rest_user_fills.json",
                [](std::string_view f) {
                    static FillBuffer storage;
                    (void)parse_fills(f, storage);
                }},
               {"rest_clearinghouse_state.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::hyperliquid
