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
               {"ws_orderbook_delta.json",
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
               {"ws_ticker_delta_mark.json",
                [](std::string_view f) {
                    (void)parse_topic(f);
                    (void)parse_ticker(f);
                }},
               {"ws_refused.json",
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
           });
}

} // namespace
} // namespace raikiri::venues::apex
