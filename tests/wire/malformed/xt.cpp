#include <raikiri/venues/xt/book.hpp>
#include <raikiri/venues/xt/catalogue.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/market_data.hpp>
#include <raikiri/venues/xt/orders.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::xt {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Xt) {
    hammer("xt",
           {
               {"ws_depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels, MarketType::Perp);
                }},
               {"ws_depth_update.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth_update(f, levels);
                }},
               {"ws_spot_depth.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_depth(f, levels, MarketType::Spot);
                }},
               {"ws_trade_buy.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_trade(f, MarketType::Perp);
                }},
               {"ws_spot_public_trade.json", [](std::string_view f) { (void)parse_trade(f, MarketType::Spot); }},
               {"ws_mark_price.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_mark_price(f);
                }},
               {"ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f, MarketType::Perp);
                }},
               {"ws_subscribe_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_error_unknown_topic.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_ack(f);
                }},
               {"ws_private_ack.json", [](std::string_view f) { (void)parse_private_frame(f); }},
               {"ws_futures_order_new.json", [](std::string_view f) { (void)parse_futures_order(f); }},
               {"ws_spot_order_new.json", [](std::string_view f) { (void)parse_spot_order_push(f); }},
               {"rest_symbol_list.json",
                [](std::string_view f) {
                    static InstrumentBuffer instruments;
                    (void)parse_rest_envelope(f);
                    (void)parse_instruments(f, instruments);
                }},
               {"rest_error_invalid_symbol.json", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_spot_error_auth.json", [](std::string_view f) { (void)parse_spot_envelope(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::xt
