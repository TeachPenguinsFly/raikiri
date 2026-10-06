#include <raikiri/venues/paradex/wire.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "harness.hpp"

namespace raikiri::venues::paradex {
namespace {

using wire::malformed::hammer;

void every_sbe(std::string_view f) {
    static const auto levels = std::make_unique<LevelBuffer>();
    (void)is_sbe(f);
    (void)template_of(f);
    (void)parse_book(f, *levels);
    (void)parse_bbo(f);
    (void)parse_trade(f);
    (void)parse_summary(f);
    (void)parse_funding(f);
}

TEST(Malformed, Paradex) {
    hammer("paradex",
           {
               {"rest_markets.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<MarketBuffer>();
                    (void)parse_markets(f, *rows);
                }},
               {"rest_time.json", [](std::string_view f) { (void)parse_reply(f); }},
               {"ws_reply_*",
                [](std::string_view f) {
                    (void)is_sbe(f);
                    (void)parse_reply(f);
                }},
               {"sbe_*", every_sbe},
           });
}

} // namespace
} // namespace raikiri::venues::paradex
