#include <raikiri/venues/dydx/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::dydx {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Dydx) {
    hammer("dydx",
           {
               {"rest_perpetual_markets.json",
                [](std::string_view f) {
                    static MarketBuffer markets;
                    (void)parse_perpetual_markets(f, markets);
                }},
               {"ws_book_snapshot.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_book_snapshot_shallow.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_book_batch.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_book_update.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_trades_subscribed.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_trades_update.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_trades_batch.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_error_limit.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_connected.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
               {"ws_unsubscribed.json",
                [](std::string_view f) {
                    static FrameBuffer storage;
                    (void)parse_frame(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::dydx
