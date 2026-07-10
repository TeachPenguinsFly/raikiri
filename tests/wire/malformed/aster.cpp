#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/market_data.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::aster {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Aster) {
    hammer("aster",
           {
               {"ws_book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_book_ticker(*inner);
                    }
                }},
               {"ws_agg_trade.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_trade(*inner);
                    }
                }},
               {"ws_mark_price.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_mark_price(*inner);
                    }
                }},
               {"ws_depth_update.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_depth_update(*inner, storage);
                    }
                }},
               {"ws_depth_update_next.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_depth_update(*inner, storage);
                    }
                }},
               {"rest_depth_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_depth_snapshot(f, storage);
                }},
               {"rest_exchange_info.json",
                [](std::string_view f) {
                    static InstrumentBuffer storage;
                    (void)parse_exchange_info(f, storage);
                }},
               {"rest_funding_info.json",
                [](std::string_view f) {
                    static FundingInfoBuffer storage;
                    (void)parse_funding_info(f, storage);
                }},
               {"rest_premium_index.json",
                [](std::string_view f) {
                    static PremiumBuffer storage;
                    (void)parse_premium_index(f, storage);
                }},
               {"rest_server_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_ticker_24h.json",
                [](std::string_view f) {
                    static Ticker24hBuffer storage;
                    (void)parse_ticker_24h(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::aster
