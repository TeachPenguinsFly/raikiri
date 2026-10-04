#include <raikiri/venues/derive/wire.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "harness.hpp"

namespace raikiri::venues::derive {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Derive) {
    hammer("derive",
           {
               {"ws_orderbook_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_head(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_ticker_slim_*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_ticker(f);
                }},
               {"ws_trades_perp.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_head(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_subscribe_ack*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_subscribe_ack(f);
                    (void)parse_rpc_error(f);
                }},
               {"rest_get_all_instruments_*",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<InstrumentBuffer>();
                    (void)parse_instruments(f, *rows);
                    (void)parse_rpc_error(f);
                }},
               {"rest_error_*",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<InstrumentBuffer>();
                    (void)parse_rpc_error(f);
                    (void)parse_instruments(f, *rows);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::derive
