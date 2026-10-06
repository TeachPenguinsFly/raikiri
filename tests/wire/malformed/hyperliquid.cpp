#include <raikiri/venues/hyperliquid/account.hpp>
#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
#include <raikiri/venues/hyperliquid/post.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::hyperliquid {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Hyperliquid) {
    hammer("hyperliquid",
           {
               {"ws_*bbo.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_*trades*",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_*l2book*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_*active_asset_ctx.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_asset_ctx(f);
                }},
               {"ws_all_mids.json", [](std::string_view f) { (void)parse_event(f); }},
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
               {"rest_meta_dex_*",
                [](std::string_view f) {
                    static AssetBuffer storage;
                    (void)parse_meta(f, storage);
                }},
               {"rest_null_body.json",
                [](std::string_view f) {
                    static AssetBuffer storage;
                    (void)parse_meta(f, storage);
                }},
               {"rest_meta_and_asset_ctxs*",
                [](std::string_view f) {
                    static AssetBuffer assets;
                    static AssetCtxBuffer storage;
                    (void)parse_meta(f, assets);
                    (void)parse_asset_ctxs(f, storage);
                }},
               {"rest_spot_meta_and_asset_ctxs.json",
                [](std::string_view f) {
                    static SpotCtxBuffer storage;
                    (void)parse_spot_asset_ctxs(f, storage);
                }},
               {"rest_spot_meta.json",
                [](std::string_view f) {
                    static SpotAssetBuffer storage;
                    (void)parse_spot_meta(f, storage);
                    (void)parse_token_name(f, 360);
                }},
               {"rest_perp_dexs.json",
                [](std::string_view f) {
                    static PerpDexBuffer storage;
                    (void)parse_perp_dexs(f, storage);
                }},
               {"rest_perp_categories.json",
                [](std::string_view f) {
                    static PerpCategoryBuffer storage;
                    (void)parse_perp_categories(f, storage);
                }},
               {"exchange_*",
                [](std::string_view f) {
                    static ExchangeStatusBuffer storage;
                    (void)parse_exchange_reply(f, storage);
                }},
               {"rest_*open_orders.json",
                [](std::string_view f) {
                    static OpenOrderBuffer storage;
                    (void)parse_open_orders(f, storage);
                }},
               {"rest_order_status*", [](std::string_view f) { (void)parse_order_status(f); }},
               {"rest_user_fills.json",
                [](std::string_view f) {
                    static FillBuffer storage;
                    (void)parse_fills(f, storage);
                }},
               {"rest_user_funding.json",
                [](std::string_view f) {
                    static FundingChargeBuffer storage;
                    (void)parse_user_funding(f, storage);
                }},
               {"rest_*clearinghouse_state.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
               {"ws_post_*",
                [](std::string_view f) {
                    static ExchangeStatusBuffer storage;
                    if (const auto post = parse_post_reply(f); post.has_value()) {
                        (void)parse_exchange_reply(post->payload, storage);
                    }
                }},
           });
}

} // namespace
} // namespace raikiri::venues::hyperliquid
