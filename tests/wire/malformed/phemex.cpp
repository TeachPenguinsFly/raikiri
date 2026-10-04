#include <raikiri/venues/phemex/book.hpp>
#include <raikiri/venues/phemex/catalogue.hpp>
#include <raikiri/venues/phemex/envelope.hpp>
#include <raikiri/venues/phemex/market_data.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::phemex {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Phemex) {
    hammer("phemex",
           {
               {"ws_real_book_*",
                [](std::string_view f) {
                    static LevelBuffer<WireRealLevel> levels;
                    (void)parse_event(f);
                    (void)parse_real_book(f, levels);
                }},
               {"ws_spot_book_*",
                [](std::string_view f) {
                    static LevelBuffer<WireScaledLevel> levels;
                    (void)parse_event(f);
                    (void)parse_scaled_book(f, levels);
                }},
               {"ws_inverse_book_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer<WireScaledLevel> levels;
                    (void)parse_event(f);
                    (void)parse_scaled_book(f, levels);
                }},
               {"ws_real_trades*",
                [](std::string_view f) {
                    static PrintBuffer<WireRealPrint> prints;
                    (void)parse_event(f);
                    (void)parse_real_trades(f, prints);
                }},
               {"ws_spot_trades.json",
                [](std::string_view f) {
                    static PrintBuffer<WireScaledPrint> prints;
                    (void)parse_event(f);
                    (void)parse_scaled_trades(f, prints);
                }},
               {"ws_inverse_trades.json",
                [](std::string_view f) {
                    static PrintBuffer<WireScaledPrint> prints;
                    (void)parse_event(f);
                    (void)parse_scaled_trades(f, prints);
                }},
               {"ws_perp_pack_*",
                [](std::string_view f) {
                    static PackColumns columns;
                    static PackBuffer rows;
                    (void)parse_event(f);
                    (void)parse_perp_pack(f, columns, rows);
                }},
               {"ws_inverse_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_inverse_ticker(f);
                }},
               {"ws_spot_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_spot_ticker(f);
                }},
               {"ws_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error_subscription_limit.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"rest_products.json",
                [](std::string_view f) {
                    static CatalogueBuffer catalogue;
                    (void)parse_products(f, catalogue);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::phemex
