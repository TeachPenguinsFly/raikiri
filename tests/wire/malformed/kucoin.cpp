#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/market_data.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>
#include <raikiri/venues/kucoin/trade.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::kucoin {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Kucoin) {
    hammer("kucoin",
           {
               {"rest_funding_history*",
                [](std::string_view f) {
                    static FundingChargeBuffer storage;
                    (void)parse_funding_history(f, storage);
                }},
               {"rest_history_positions.json",
                [](std::string_view f) {
                    static ClosedPositionBuffer storage;
                    (void)parse_history_positions(f, storage);
                }},
               {"ws_trade_*", [](std::string_view f) { (void)parse_trade_reply(f); }},
               {"ws_ticker_v2.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f, MarketType::Perp);
                }},
               {"ws_spot_level1.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f, MarketType::Spot);
                }},
               {"ws_execution.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_trade(f, MarketType::Perp);
                }},
               {"ws_spot_match.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_trade(f, MarketType::Spot);
                }},
               {"ws_level2*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_change(f);
                }},
               {"ws_spot_level2depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book_snapshot(f, levels);
                }},
               {"ws_uta_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_uta_book(f, levels);
                    (void)parse_uta_trade(f);
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
               {"ws_error_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_private_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_welcome.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_futures_order_*",
                [](std::string_view f) {
                    (void)parse_private_event(f);
                    (void)parse_order_change(f);
                }},
               {"ws_spot_order_*",
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
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_order_ack.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_timestamp.json",
                [](std::string_view f) {
                    (void)parse_rest_envelope(f);
                    (void)parse_server_time(f);
                }},
               {"rest_contracts_*",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_contracts(f, storage);
                }},
               {"rest_spot_symbols.json",
                [](std::string_view f) {
                    static SpotSymbolBuffer storage;
                    (void)parse_spot_symbols(f, storage);
                }},
               {"rest_spot_all_tickers.json",
                [](std::string_view f) {
                    static SpotTickerBuffer storage;
                    (void)parse_spot_tickers(f, storage);
                }},
               {"rest_spot_active_symbols.json",
                [](std::string_view f) {
                    static SymbolBuffer storage;
                    (void)parse_active_symbols(f, storage);
                }},
               {"rest_funding_rate_current.json", [](std::string_view f) { (void)parse_funding_current(f); }},
               {"rest_*_fills.json",
                [](std::string_view f) {
                    static FillBuffer storage;
                    (void)parse_fills(f, storage);
                }},
               {"rest_spot_orders_active.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_spot_orders(f, storage);
                }},
               {"rest_futures_orders_active.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_futures_orders(f, storage);
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
