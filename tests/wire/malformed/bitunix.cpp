#include <raikiri/venues/bitunix/orders.hpp>
#include <raikiri/venues/bitunix/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::bitunix {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bitunix) {
    hammer("bitunix",
           {
               {"ws_depth_book*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_head(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_trade.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_head(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_connect.json", [](std::string_view f) { (void)parse_head(f); }},
               {"rest_trading_pairs.json",
                [](std::string_view f) {
                    static PairBuffer pairs;
                    (void)parse_status(f);
                    (void)parse_pairs(f, pairs);
                }},
               {"rest_rate_limited.json", [](std::string_view f) { (void)parse_status(f); }},
               {"spot_rest_coin_pairs.json",
                [](std::string_view f) {
                    static PairBuffer pairs;
                    (void)parse_status(f);
                    (void)parse_spot_pairs(f, pairs);
                }},
               {"spot_rest_depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_spot_depth(f, levels);
                }},
               {"rest_funding_batch.json",
                [](std::string_view f) {
                    static FundingBuffer rows;
                    (void)parse_funding(f, rows);
                }},
               {"rest_tickers.json",
                [](std::string_view f) {
                    static TickerBuffer rows;
                    (void)parse_tickers(f, rows);
                }},
               {"doc_*", [](std::string_view f) { (void)parse_status(f); }},
               {"doc_*_order.json", [](std::string_view f) { (void)parse_ack(f); }},
               {"doc_cancel_orders.json",
                [](std::string_view f) {
                    static CancelBuffer cancels;
                    (void)parse_cancels(f, cancels);
                }},
               {"doc_pending_orders.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
               {"doc_history_trades.json",
                [](std::string_view f) {
                    static FillBuffer fills;
                    (void)parse_fills(f, fills);
                }},
               {"doc_pending_positions.json",
                [](std::string_view f) {
                    static PositionBuffer positions;
                    (void)parse_positions(f, positions);
                }},
               {"doc_account.json",
                [](std::string_view f) {
                    static BalanceBuffer balances;
                    (void)parse_balances(f, balances);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::bitunix
