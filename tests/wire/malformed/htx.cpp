#include <raikiri/venues/htx/book.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/market_data.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::htx {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Htx) {
    hammer("htx",
           {
               {"ws_bbo.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_depth_step0_full.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_trades_batch.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_trades(f, trades);
                }},
               {"ws_detail.json", [](std::string_view f) { (void)parse_detail(f); }},
               {"ws_notify_funding.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_funding(f);
                }},
               {"ws_error.json", [](std::string_view f) { (void)parse_stream_error(f); }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::htx
