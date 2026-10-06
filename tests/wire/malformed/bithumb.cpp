#include <raikiri/venues/upbit/wire.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "harness.hpp"

namespace raikiri::venues::upbit {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Bithumb) {
    hammer("bithumb",
           {
               {"ws_orderbook_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_kind(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_trade_*",
                [](std::string_view f) {
                    (void)parse_kind(f);
                    (void)parse_trade(f);
                }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_kind(f);
                    (void)parse_refusal(f);
                }},
               {"rest_market_all.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<MarketBuffer>();
                    (void)parse_markets(f, *rows);
                }},
               {"rest_ticker*",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<TickerBuffer>();
                    (void)parse_tickers(f, *rows);
                }},
               {"rest_orderbook_krw_btc.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<TickerBuffer>();
                    (void)parse_tickers(f, *rows);
                    (void)parse_refusal(f);
                }},
               {"rest_error_*",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<TickerBuffer>();
                    (void)parse_refusal(f);
                    (void)parse_tickers(f, *rows);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::upbit
