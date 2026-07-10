#include <raikiri/venues/mexc/book.hpp>
#include <raikiri/venues/mexc/catalogue.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/market_data.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/positions.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::mexc {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Mexc) {
    hammer("mexc",
           {
               {"ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_deal.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_depth_full.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    static LevelBuffer levels_two;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                    (void)parse_event_and_depth(f, levels_two);
                }},
               {"ws_depth_update.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_personal_order_new.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_futures_order(f);
                }},
               {"ws_personal_deal.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_futures_deal(f);
                }},
               {"ws_personal_position.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_futures_position(f);
                }},
               {"rest_contract_detail.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_error_bad_contract.json",
                [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_server_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_funding_rate.json", [](std::string_view f) { (void)parse_funding_rate(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::mexc
