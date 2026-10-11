#include <raikiri/venues/edgex/trade.hpp>
#include <raikiri/venues/edgex/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::edgex {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Edgex) {
    hammer("edgex",
           {
               {"ws_depth_*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_spot_depth_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_trades_*",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_event(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_spot_trades_changed.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_event(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_*ticker_all.json",
                [](std::string_view f) {
                    static TickerBuffer tickers;
                    (void)parse_event(f);
                    (void)parse_tickers(f, tickers);
                }},
               {"ws_ping.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ping(f);
                }},
               {"ws_subscribed.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error_*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_funding_rate_all.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_meta_perp.json",
                [](std::string_view f) {
                    static ContractBuffer contracts;
                    (void)parse_perp_catalogue(f, contracts);
                    (void)parse_signing_meta(f);
                }},
               {"ws_private_*",
                [](std::string_view f) {
                    static TradeBuffer rows;
                    (void)parse_private_kind(f);
                    (void)parse_trade_event(f, rows);
                }},
               {"rest_create_*", [](std::string_view f) { (void)parse_reply(f); }},
               {"rest_latest_funding_rate.json",
                [](std::string_view f) {
                    static ContractBuffer contracts;
                    (void)parse_perp_catalogue(f, contracts);
                }},
               {"rest_meta_spot.json",
                [](std::string_view f) {
                    static ContractBuffer contracts;
                    (void)parse_spot_catalogue(f, contracts);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::edgex
