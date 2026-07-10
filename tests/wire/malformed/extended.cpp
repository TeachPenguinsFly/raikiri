#include <raikiri/venues/extended/book.hpp>
#include <raikiri/venues/extended/catalogue.hpp>
#include <raikiri/venues/extended/market_data.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::extended {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Extended) {
    hammer("extended",
           {
               {"ws_book_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_book(f, levels);
                }},
               {"ws_book_delta.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_book(f, levels);
                }},
               {"ws_book_delisted.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_book(f, levels);
                }},
               {"ws_top.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_book(f, levels);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    static PrintBuffer trades;
                    (void)parse_trades(f, trades);
                }},
               {"ws_trades_backlog.json",
                [](std::string_view f) {
                    static PrintBuffer trades;
                    (void)parse_trades(f, trades);
                }},
               {"ws_funding.json", [](std::string_view f) { (void)parse_funding(f); }},
               {"ws_funding_ghost.json", [](std::string_view f) { (void)parse_funding(f); }},
               {"ws_mark.json", [](std::string_view f) { (void)parse_mark(f); }},
               {"rest_markets.json",
                [](std::string_view f) {
                    static MarketBuffer storage;
                    (void)parse_markets(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::extended
