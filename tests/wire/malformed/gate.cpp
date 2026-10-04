#include <raikiri/venues/gate/account.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/catalogue.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/positions.hpp>
#include <raikiri/venues/gate/trade.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::gate {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Gate) {
    hammer("gate",
           {
               {"rest_option_contracts.json",
                [](std::string_view f) {
                    static OptionContractBuffer storage;
                    (void)parse_option_contracts(f, storage);
                }},
               {"rest_option_underlyings.json",
                [](std::string_view f) {
                    static OptionUnderlyingBuffer storage;
                    (void)parse_option_underlyings(f, storage);
                }},
               {"ws_option_contract_tickers.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_option_ticker(f);
                }},
               {"ws_option_book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_ticker(f);
                }},
               {"ws_option_trades.json",
                [](std::string_view f) {
                    static TradeBuffer storage;
                    (void)parse_event(f);
                    (void)parse_trades(f, storage, MarketType::Option);
                }},
               {"ws_api_*", [](std::string_view f) { (void)parse_trade_reply(f); }},
               {"ws_book_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_book_ticker(f);
                }},
               {"ws_order_book_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book_update(f, levels);
                }},
               {"ws_spot_order_book.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_book_snapshot(f, levels);
                }},
               {"ws_trades*",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades, MarketType::Perp);
                }},
               {"ws_spot_public_trades.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades, MarketType::Spot);
                }},
               {"ws_tickers.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_pong.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_order_book*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_order_book(f, levels);
                }},
               {"rest_contracts.json",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_contracts(f, storage);
                }},
               {"rest_currency_pairs.json",
                [](std::string_view f) {
                    static CurrencyPairBuffer storage;
                    (void)parse_currency_pairs(f, storage);
                }},
               {"rest_futures_tickers.json",
                [](std::string_view f) {
                    static TickerQuoteBuffer storage;
                    (void)parse_futures_tickers(f, storage);
                }},
               {"rest_server_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"ws_futures_order_*",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_ws_orders(f, storage, MarketType::Perp);
                }},
               {"ws_spot_order_[!b]*",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_ws_orders(f, storage, MarketType::Spot);
                }},
               {"ws_futures_usertrade.json",
                [](std::string_view f) {
                    static UserTradeBuffer storage;
                    (void)parse_ws_user_trades(f, storage, MarketType::Perp);
                }},
               {"ws_spot_usertrade.json",
                [](std::string_view f) {
                    static UserTradeBuffer storage;
                    (void)parse_ws_user_trades(f, storage, MarketType::Spot);
                }},
               {"rest_futures_open_orders.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_rest_orders(f, storage, MarketType::Perp);
                }},
               {"rest_spot_open_orders.json",
                [](std::string_view f) {
                    static OrderBuffer storage;
                    (void)parse_rest_orders(f, storage, MarketType::Spot);
                }},
               {"rest_futures_order.json", [](std::string_view f) { (void)parse_rest_order(f, MarketType::Perp); }},
               {"rest_futures_my_trades.json",
                [](std::string_view f) {
                    static UserTradeBuffer storage;
                    (void)parse_rest_user_trades(f, storage, MarketType::Perp);
                }},
               {"rest_spot_my_trades.json",
                [](std::string_view f) {
                    static UserTradeBuffer storage;
                    (void)parse_rest_user_trades(f, storage, MarketType::Spot);
                }},
               {"rest_futures_positions.json",
                [](std::string_view f) {
                    static PositionBuffer storage;
                    (void)parse_positions(f, storage);
                }},
               {"rest_futures_account.json", [](std::string_view f) { (void)parse_account_user_id(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::gate
