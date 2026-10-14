#include <raikiri/venues/apex/trade.hpp>
#include <raikiri/venues/apex/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::apex {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Apex) {
    hammer("apex",
           {
               {"rest_symbols.json",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_symbols(f, storage);
                }},
               {"ws_orderbook_snapshot.json",
                [](std::string_view f) {
                    static OrderbookBuffer storage;
                    (void)parse_topic(f);
                    (void)parse_orderbook(f, storage);
                }},
               {"ws_orderbook_delta*",
                [](std::string_view f) {
                    static OrderbookBuffer storage;
                    (void)parse_topic(f);
                    (void)parse_orderbook(f, storage);
                }},
               {"ws_trade_snapshot.json",
                [](std::string_view f) {
                    static TradeBuffer storage;
                    (void)parse_topic(f);
                    (void)parse_trades(f, storage);
                }},
               {"ws_trade_delta.json",
                [](std::string_view f) {
                    static TradeBuffer storage;
                    (void)parse_topic(f);
                    (void)parse_trades(f, storage);
                }},
               {"ws_ticker_snapshot.json",
                [](std::string_view f) {
                    (void)parse_topic(f);
                    (void)parse_ticker(f);
                }},
               {"ws_ticker_delta*",
                [](std::string_view f) {
                    (void)parse_topic(f);
                    (void)parse_ticker(f);
                }},
               {"ws_refused*",
                [](std::string_view f) {
                    static AckBuffer storage;
                    (void)parse_topic(f);
                    (void)parse_ack(f, storage);
                }},
               {"ws_subscribed.json",
                [](std::string_view f) {
                    static AckBuffer storage;
                    (void)parse_topic(f);
                    (void)parse_ack(f, storage);
                }},
               {"ws_server_ping.json",
                [](std::string_view f) {
                    (void)parse_topic(f);
                    (void)parse_ping(f);
                }},
               {"rest_symbols_signing.json", [](std::string_view f) { (void)parse_signing_meta(f); }},
               {"rest_account.json",
                [](std::string_view f) {
                    static PrivateBuffer storage;
                    (void)parse_account(f, storage);
                }},
               {"rest_*order*",
                [](std::string_view f) {
                    static PrivateBuffer storage;
                    (void)parse_reply(f);
                    (void)parse_rest_rows(f, storage, false);
                }},
               {"rest_fills.json",
                [](std::string_view f) {
                    static PrivateBuffer storage;
                    (void)parse_reply(f);
                    (void)parse_rest_rows(f, storage, true);
                }},
               {"rest_error_*", [](std::string_view f) { (void)parse_reply(f); }},
               {"ws_account_*",
                [](std::string_view f) {
                    static PrivateBuffer storage;
                    (void)parse_private_kind(f);
                    (void)parse_account_push(f, storage);
                }},
               {"ws_private_ping.json", [](std::string_view f) { (void)parse_private_kind(f); }},
               {"ws_login_ack.json", [](std::string_view f) { (void)parse_private_kind(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::apex
