#include <raikiri/venues/edgex/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::edgex {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Edgex) {
    hammer("edgex",
           {
               {"ws_depth_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_depth_changed.json",
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
               {"ws_trades_snapshot.json",
                [](std::string_view f) {
                    static PrintBuffer prints;
                    (void)parse_event(f);
                    (void)parse_trades(f, prints);
                }},
               {"ws_trades_changed.json",
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
               {"ws_ticker_all.json",
                [](std::string_view f) {
                    static TickerBuffer tickers;
                    (void)parse_event(f);
                    (void)parse_tickers(f, tickers);
                }},
               {"ws_spot_ticker_all.json",
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
               {"ws_error_contract.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_error_channel.json", [](std::string_view f) { (void)parse_stream_error(f); }},
               {"ws_funding_rate_all.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_meta_perp.json",
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
