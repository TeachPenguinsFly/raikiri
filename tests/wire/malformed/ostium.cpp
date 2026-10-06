#include <raikiri/venues/ostium/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::ostium {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Ostium) {
    hammer("ostium",
           {
               {"ws_snapshot.json",
                [](std::string_view f) {
                    static QuoteBuffer quotes;
                    (void)parse_frame(f);
                    (void)parse_quotes(f, quotes);
                }},
               {"ws_tick.json",
                [](std::string_view f) {
                    static QuoteBuffer quotes;
                    (void)parse_frame(f);
                    (void)parse_quotes(f, quotes);
                }},
               {"ws_ack.json", [](std::string_view f) { (void)parse_frame(f); }},
               {"ws_error.json", [](std::string_view f) { (void)parse_frame(f); }},
               {"rest_pairs.json",
                [](std::string_view f) {
                    static PairBuffer pairs;
                    (void)parse_pairs(f, pairs);
                }},
               {"rest_subgraph_pairs.json",
                [](std::string_view f) {
                    static RolloverBuffer rows;
                    (void)parse_rollover(f, rows);
                }},
               {"rest_subgraph_orders.json",
                [](std::string_view f) {
                    static FillBuffer fills;
                    (void)parse_fills(f, fills);
                }},
               {"rest_subgraph_error.json",
                [](std::string_view f) {
                    static FillBuffer fills;
                    (void)parse_fills(f, fills);
                }},
               {"rest_status.json", [](std::string_view) {}},
           });
}

} // namespace
} // namespace raikiri::venues::ostium
