#include <raikiri/venues/lighter/account.hpp>
#include <raikiri/venues/lighter/book.hpp>
#include <raikiri/venues/lighter/catalogue.hpp>
#include <raikiri/venues/lighter/envelope.hpp>
#include <raikiri/venues/lighter/market_data.hpp>

#include <gtest/gtest.h>

#include <vector>

#include "harness.hpp"

namespace raikiri::venues::lighter {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Lighter) {
    hammer("lighter",
           {
               {"ws_order_book_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_order_book(f, levels);
                }},
               {"ws_order_book_update.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_order_book(f, levels);
                }},
               {"ws_order_book_update_next.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_order_book(f, levels);
                }},
               {"ws_trade_snapshot.json",
                [](std::string_view f) {
                    static PrintBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_trade_update.json",
                [](std::string_view f) {
                    static PrintBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_market_stats.json",
                [](std::string_view f) {
                    static StatsBuffer storage;
                    (void)parse_event(f);
                    (void)parse_market_stats(f, storage);
                }},
               {"ws_market_stats_all.json",
                [](std::string_view f) {
                    static StatsBuffer storage;
                    (void)parse_event(f);
                    (void)parse_market_stats(f, storage);
                }},
               {"ws_spot_market_stats.json",
                [](std::string_view f) {
                    static StatsBuffer storage;
                    (void)parse_event(f);
                    (void)parse_market_stats(f, storage);
                }},
               {"ws_error_rate_limit.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_error_too_many_subscriptions.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"rest_order_book_details.json",
                [](std::string_view f) {
                    static MarketBuffer storage;
                    (void)parse_order_book_details(f, storage);
                }},
               {"rest_order_book_details_tradfi.json",
                [](std::string_view f) {
                    static MarketBuffer storage;
                    (void)parse_order_book_details(f, storage);
                }},
               {"rest_account.json",
                [](std::string_view f) {
                    static WireAccount account;
                    (void)parse_account(f, account);
                }},
               {"rest_next_nonce.json", [](std::string_view f) { (void)parse_next_nonce(f); }},
               {"rest_send_tx_api_key_not_found.json", [](std::string_view f) { (void)parse_send_reply(f); }},
               {"rest_send_tx_invalid_signature.json", [](std::string_view f) { (void)parse_send_reply(f); }},
               {"ws_send_tx_invalid_signature.json", [](std::string_view f) { (void)parse_send_reply(f); }},
               {"rest_trades.json",
                [](std::string_view f) {
                    static std::vector<WireTrade> trades;
                    (void)parse_account_trades(f, trades);
                }},
               {"ws_account_all_trades_subscribed.json",
                [](std::string_view f) {
                    static std::vector<WireTrade> trades;
                    (void)parse_account_trades(f, trades);
                }},
               {"ws_account_auth_expired.json",
                [](std::string_view f) {
                    (void)parse_stream_error(f);
                    (void)parse_send_reply(f);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::lighter
