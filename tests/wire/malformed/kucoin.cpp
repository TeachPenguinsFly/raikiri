#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/market_data.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::kucoin {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Kucoin) {
    hammer("kucoin",
           {
               {"ws_ticker_v2.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f, MarketType::Perp);
                }},
               {"ws_execution.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_trade(f, MarketType::Perp);
                }},
               {"ws_level2.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_change(f);
                }},
               {"ws_mark_index_price.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_mark_price(f);
                }},
               {"ws_funding_rate.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_funding_rate(f);
                }},
               {"ws_error_topic.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_welcome.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_futures_order_match.json",
                [](std::string_view f) {
                    (void)parse_private_event(f);
                    (void)parse_order_change(f);
                }},
               {"rest_level2_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_level2_snapshot(f, levels);
                }},
               {"rest_bullet_public.json", [](std::string_view f) { (void)parse_bullet(f); }},
               {"rest_error_unknown_symbol.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_contracts_active.json",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_contracts(f, storage);
                }},
               {"rest_futures_fills.json",
                [](std::string_view f) {
                    static FillBuffer storage;
                    (void)parse_fills(f, storage);
                }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
               {"rest_account_overview.json", [](std::string_view f) { (void)parse_account_overview(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::kucoin
