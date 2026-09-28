#include <raikiri/venues/bitunix/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::bitunix {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bitunix) {
    hammer("bitunix",
           {
               {"ws_depth_book15.json",
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
               {"rest_trading_pairs.json",
                [](std::string_view f) {
                    static PairBuffer pairs;
                    (void)parse_status(f);
                    (void)parse_pairs(f, pairs);
                }},
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
           });
}

} // namespace
} // namespace raikiri::venues::bitunix
