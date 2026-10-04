#include <raikiri/venues/toobit/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::toobit {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Toobit) {
    hammer("toobit",
           {
               {"ws_bbo*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_bbo(f);
                }},
               {"ws_depth_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_head(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_trade*",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_head(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_mark.json",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_mark(f);
                }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_error.json", [](std::string_view f) { (void)parse_head(f); }},
               {"user_*",
                [](std::string_view f) {
                    static ExecutionBuffer executions;
                    (void)parse_private(f, executions);
                }},
               {"rest_*", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"rest_exchange_info.json",
                [](std::string_view f) {
                    static MarketBuffer markets;
                    (void)parse_exchange_info(f, markets);
                }},
               {"rest_funding_rate.json",
                [](std::string_view f) {
                    static FundingBuffer rows;
                    (void)parse_funding_rates(f, rows);
                }},
               {"rest_contract_ticker.json",
                [](std::string_view f) {
                    static TickerBuffer rows;
                    (void)parse_tickers(f, rows);
                }},
               {"rest_listen_key.json", [](std::string_view f) { (void)parse_listen_key(f); }},
               {"rest_order.json", [](std::string_view f) { (void)parse_order(f); }},
               {"rest_open_orders.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
               {"rest_user_trades.json",
                [](std::string_view f) {
                    static UserTradeBuffer trades;
                    (void)parse_user_trades(f, trades);
                }},
               {"rest_positions.json",
                [](std::string_view f) {
                    static PositionBuffer positions;
                    (void)parse_positions(f, positions);
                }},
               {"rest_balance.json",
                [](std::string_view f) {
                    static BalanceBuffer balances;
                    (void)parse_balances(f, balances);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::toobit
