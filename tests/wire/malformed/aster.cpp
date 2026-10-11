#include <raikiri/venues/aster/account.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/market_data.hpp>
#include <raikiri/venues/aster/orders.hpp>
#include <raikiri/venues/aster/positions.hpp>

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
               {"spot_ws_book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_book_ticker(*inner);
                    }
                }},
               {"spot_ws_trade.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_trade(*inner);
                    }
                }},
               {"spot_ws_depth_update*",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_event(f);
                    if (const auto inner = unwrap(f); inner.has_value()) {
                        (void)parse_depth_update(*inner, storage);
                    }
                }},
               {"spot_rest_depth_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_depth_snapshot(f, storage);
                }},
               {"spot_rest_exchange_info.json",
                [](std::string_view f) {
                    static InstrumentBuffer storage;
                    (void)parse_exchange_info(f, storage);
                }},
               {"spot_rest_ticker_24h.json",
                [](std::string_view f) {
                    static Ticker24hBuffer storage;
                    (void)parse_ticker_24h(f, storage);
                }},
               {"spot_rest_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_depth_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer storage;
                    (void)parse_depth_snapshot(f, storage);
                }},
               {"rest_exchange_info*",
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
               {"user_order_trade_update.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_order_update(f);
                }},
               {"user_listen_key_expired.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_listen_key.json", [](std::string_view f) { (void)parse_listen_key(f); }},
               {"rest_query_order.json", [](std::string_view f) { (void)parse_order(f); }},
               {"rest_open_orders.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_orders(f, storage);
                }},
               {"rest_user_trades.json",
                [](std::string_view f) {
                    static TradeBuffer storage;
                    (void)parse_user_trades(f, storage);
                }},
               {"rest_position_risk.json",
                [](std::string_view f) {
                    std::array<WirePositionRisk, 8> storage{};
                    (void)parse_position_risk(f, storage);
                }},
               {"rest_account.json",
                [](std::string_view f) {
                    static BalanceBuffer storage;
                    (void)parse_account(f, storage);
                }},
               {"rest_income.json",
                [](std::string_view f) {
                    static IncomeBuffer storage;
                    (void)parse_income(f, storage);
                }},
               {"rest_error_signature.json", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"rest_error_no_nonce.json", [](std::string_view f) { (void)parse_rest_error(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::aster
