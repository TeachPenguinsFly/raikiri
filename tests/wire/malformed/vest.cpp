#include <raikiri/venues/vest/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::vest {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Vest) {
    hammer("vest",
           {
               {"ws_depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_channel(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    (void)parse_channel(f);
                    (void)parse_trade(f);
                }},
               {"ws_subscribe.json",
                [](std::string_view f) {
                    (void)parse_channel(f);
                    (void)count_refusals(f);
                }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_channel(f); }},
               {"ws_ping_ack.json", [](std::string_view f) { (void)parse_channel(f); }},
               {"rest_exchange_info.json",
                [](std::string_view f) {
                    static MarketBuffer markets;
                    (void)parse_markets(f, markets);
                }},
               {"rest_ticker_latest.json",
                [](std::string_view f) {
                    static TickerBuffer rows;
                    (void)parse_tickers(f, rows);
                }},
               {"rest_ticker_24hr.json",
                [](std::string_view f) {
                    static StatBuffer rows;
                    (void)parse_volumes(f, rows);
                }},
               {"rest_oi.json",
                [](std::string_view f) {
                    static StatBuffer rows;
                    (void)parse_open_interest(f, rows);
                }},
               {"rest_ntp.json", [](std::string_view) {}},
           });
}

} // namespace
} // namespace raikiri::venues::vest
