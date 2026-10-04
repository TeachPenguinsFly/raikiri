#include <raikiri/venues/cryptocom/book.hpp>
#include <raikiri/venues/cryptocom/catalogue.hpp>
#include <raikiri/venues/cryptocom/envelope.hpp>
#include <raikiri/venues/cryptocom/market_data.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::cryptocom {
namespace {

using wire::malformed::hammer;

TEST(Malformed, CryptoCom) {
    hammer("cryptocom",
           {
               {"ws_heartbeat.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_book_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_book_[ps]*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, true, levels);
                    (void)parse_book(f, false, levels);
                }},
               {"ws_book_update_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, true, levels);
                    (void)parse_book(f, false, levels);
                }},

               {"fuzz_book_trailing_comma.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, true, levels);
                    (void)parse_book(f, false, levels);
                }},
               {"fuzz_book_update_unclosed.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book(f, true, levels);
                    (void)parse_book(f, false, levels);
                }},
               {"ws_trade_*",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_ticker_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_funding_perp_initial.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_value(f);
                }},
               {"ws_estimatedfunding_perp_initial.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_value(f);
                }},
               {"ws_mark_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_value(f);
                }},
               {"ws_error_*", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_error.json", [](std::string_view f) { (void)parse_rest_status(f); }},
               {"rest_instruments.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_instruments(f, instruments);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::cryptocom
