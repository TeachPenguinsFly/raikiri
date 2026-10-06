#include <raikiri/venues/bullish/wire.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "harness.hpp"

namespace raikiri::venues::bullish {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bullish) {
    hammer("bullish",
           {
               {"rest_markets.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<MarketBuffer>();
                    (void)parse_markets(f, *rows);
                }},
               {"rest_time.json", [](std::string_view f) { (void)parse_head(f); }},
               {"rest_index_prices.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<IndexBuffer>();
                    (void)parse_index_prices(f, *rows);
                }},
               {"ws_l2_*",
                [](std::string_view f) {
                    static const auto levels = std::make_unique<LevelBuffer>();
                    (void)parse_head(f);
                    (void)parse_book(f, *levels);
                }},
               {"ws_tick_*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_tick(f);
                }},
               {"ws_trades_*",
                [](std::string_view f) {
                    static const auto prints = std::make_unique<PrintBuffer>();
                    (void)parse_head(f);
                    (void)parse_trades(f, *prints);
                }},
               {"ws_re*", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_keepalive_pong.json", [](std::string_view f) { (void)parse_head(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::bullish
