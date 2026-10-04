#include <raikiri/venues/pacifica/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::pacifica {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Pacifica) {
    hammer("pacifica",
           {
               {"ws_book.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_channel(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_bbo.json",
                [](std::string_view f) {
                    (void)parse_channel(f);
                    (void)parse_bbo(f);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_channel(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_prices.json",
                [](std::string_view f) {
                    static PriceBuffer prices;
                    (void)parse_channel(f);
                    (void)parse_prices(f, prices);
                }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_channel(f); }},
               {"ws_subscribe.json", [](std::string_view f) { (void)parse_channel(f); }},
               {"ws_error.json", [](std::string_view f) { (void)parse_channel(f); }},
               {"rest_info*",
                [](std::string_view f) {
                    static MarketBuffer markets;
                    (void)parse_markets(f, markets);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::pacifica
