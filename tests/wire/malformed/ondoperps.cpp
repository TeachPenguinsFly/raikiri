#include <raikiri/venues/ondoperps/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::ondoperps {
namespace {

using wire::malformed::hammer;

TEST(Malformed, OndoPerps) {
    hammer("ondoperps",
           {
               {"ws_top.json",
                [](std::string_view f) {
                    static BookBuffer books;
                    (void)parse_frame(f);
                    (void)parse_books(f, books);
                }},
               {"ws_depth.json",
                [](std::string_view f) {
                    static BookBuffer books;
                    (void)parse_frame(f);
                    (void)parse_books(f, books);
                }},
               {"ws_trades.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_frame(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_marks.json",
                [](std::string_view f) {
                    static MarkBuffer marks;
                    (void)parse_frame(f);
                    (void)parse_marks(f, marks);
                }},
               {"ws_funding.json",
                [](std::string_view f) {
                    static FundingBuffer rows;
                    (void)parse_frame(f);
                    (void)parse_funding(f, rows);
                }},
               {"ws_subscribed.json", [](std::string_view f) { (void)parse_frame(f); }},
               {"ws_error.json", [](std::string_view f) { (void)parse_frame(f); }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_frame(f); }},
               {"rest_markets.json",
                [](std::string_view f) {
                    static MarketBuffer markets;
                    (void)parse_markets(f, markets);
                }},
               {"rest_open_interest.json",
                [](std::string_view f) {
                    static StatBuffer rows;
                    (void)parse_open_interest(f, rows);
                }},
               {"rest_volume.json",
                [](std::string_view f) {
                    static StatBuffer rows;
                    (void)parse_volumes(f, rows);
                }},
               {"rest_depth_time.json", [](std::string_view f) { (void)server_time(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::ondoperps
