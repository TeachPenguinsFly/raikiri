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
               {"ws_orderbook_option.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_head(f);
                    (void)parse_book(f, levels);
                }},
               {"ws_ticker_slim_option.json",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_ticker(f);
                }},
               {"ws_ticker_slim_perp.json", [](std::string_view f) { (void)parse_ticker(f); }},
               {"ws_trades_perp.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_head(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_subscribe_ack_refused.json",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_subscribe_ack(f);
                    (void)parse_rpc_error(f);
                }},
               {"rest_get_all_instruments_option.json",
                [](std::string_view f) {
                    static const auto rows = std::make_unique<InstrumentBuffer>();
                    (void)parse_instruments(f, *rows);
                    (void)parse_rpc_error(f);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::derive
