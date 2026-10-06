#include <raikiri/venues/polymarketperps/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::polymarketperps {
namespace {

using wire::malformed::hammer;

TEST(Malformed, PolymarketPerps) {
    hammer("polymarketperps",
           {
               {"ws_bbo.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_bbo(f);
                }},
               {"ws_book.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_frame(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_frame(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_tickers.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_ticker(f);
                }},
               {"ws_statistics.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)parse_statistics(f);
                }},
               {"ws_answer.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)count_refusals(f);
                }},
               {"ws_pong.json",
                [](std::string_view f) {
                    (void)parse_frame(f);
                    (void)count_refusals(f);
                }},
               {"rest_instruments.json",
                [](std::string_view f) {
                    static InstrumentBuffer rows;
                    (void)parse_instruments(f, rows);
                }},
               {"rest_bbo.json",
                [](std::string_view f) {
                    static BboBuffer rows;
                    (void)parse_bbo_rows(f, rows);
                }},
               {"rest_time.json", [](std::string_view f) { (void)f; }},
           });
}

} // namespace
} // namespace raikiri::venues::polymarketperps
