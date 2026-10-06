#include <raikiri/venues/bitstamp/wire.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "harness.hpp"

namespace raikiri::venues::bitstamp {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bitstamp) {
    hammer("bitstamp",
           {
               {"ws_order_book_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_head(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_live_trades_*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_trade(f);
                }},
               {"ws_funding_rate_*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_funding(f);
                }},
               {"ws_subscription_succeeded.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_error.json", [](std::string_view f) { (void)parse_head(f); }},
               {"rest_markets.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<MarketBuffer>();
                    (void)parse_markets(f, *rows);
                }},
               {"rest_ticker.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<TickerBuffer>();
                    (void)parse_tickers(f, *rows);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::bitstamp
