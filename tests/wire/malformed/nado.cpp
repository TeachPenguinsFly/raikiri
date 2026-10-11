#include <raikiri/venues/nado/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::nado {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Nado) {
    hammer("nado",
           {
               {"ws_bbo.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_trade.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_trade(f);
                }},
               {"ws_depth_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_funding.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_funding(f);
                }},
               {"ws_subscribe_*", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_symbols.json",
                [](std::string_view f) {
                    static SymbolBuffer storage;
                    (void)parse_symbols(f, storage);
                }},
               {"rest_market_liquidity.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_liquidity(f, levels);
                }},
               {"rest_contracts.json",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_contracts(f, storage);
                }},
               {"rest_time.txt", [](std::string_view f) { (void)parse_server_time_ms(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::nado
