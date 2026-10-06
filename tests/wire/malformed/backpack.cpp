#include <raikiri/venues/backpack/book.hpp>
#include <raikiri/venues/backpack/catalogue.hpp>
#include <raikiri/venues/backpack/envelope.hpp>
#include <raikiri/venues/backpack/market_data.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::backpack {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Backpack) {
    hammer("backpack",
           {
               {"ws_book_ticker.json",
                [](std::string_view f) {
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_event_kind(*inner);
                        (void)parse_book_ticker(*inner);
                    }
                }},
               {"ws_trade.json",
                [](std::string_view f) {
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_event_kind(*inner);
                        (void)parse_trade(*inner);
                    }
                }},
               {"ws_mark_price_perp.json",
                [](std::string_view f) {
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_event_kind(*inner);
                        (void)parse_mark_price(*inner);
                    }
                }},
               {"ws_mark_price_spot.json",
                [](std::string_view f) {
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_event_kind(*inner);
                        (void)parse_mark_price(*inner);
                    }
                }},
               {"ws_depth.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_event_kind(*inner);
                        (void)parse_depth_update(*inner, storage);
                    }
                }},
               {"ws_depth_next.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_event_kind(*inner);
                        (void)parse_depth_update(*inner, storage);
                    }
                }},
               {"rest_markets.json",
                [](std::string_view f) {
                    static MarketBuffer storage;
                    (void)parse_markets(f, storage);
                }},
               {"rest_depth.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_depth_snapshot(f, storage);
                }},
               {"rest_open_interest.json",
                [](std::string_view f) {
                    static OpenInterestBuffer storage;
                    (void)parse_open_interest(f, storage);
                }},
               {"rest_tickers.json",
                [](std::string_view f) {
                    static TickerBuffer storage;
                    (void)parse_tickers(f, storage);
                }},
               {"rest_error.json", [](std::string_view f) { (void)parse_rest_error(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::backpack
