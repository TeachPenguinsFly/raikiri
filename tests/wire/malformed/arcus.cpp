#include <raikiri/venues/arcus/wire.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "harness.hpp"

namespace raikiri::venues::arcus {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Arcus) {
    hammer("arcus",
           {
               {"rest_markets.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<MarketBuffer>();
                    (void)parse_markets(f, *rows);
                }},
               {"rest_time.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_*book*",
                [](std::string_view f) {
                    static const auto levels = std::make_unique<LevelBuffer>();
                    (void)parse_head(f);
                    (void)parse_book(f, *levels);
                }},
               {"ws_*bbo*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_bbo(f);
                }},
               {"ws_*trades*",
                [](std::string_view f) {
                    static const auto prints = std::make_unique<PrintBuffer>();
                    (void)parse_head(f);
                    (void)parse_trades(f, *prints);
                }},
               {"ws_markets.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<StatsBuffer>();
                    (void)parse_head(f);
                    (void)parse_stats(f, *rows);
                }},
               {"ws_connected.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_degraded.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_error_*", [](std::string_view f) { (void)parse_head(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::arcus
