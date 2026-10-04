#include <raikiri/venues/lbank/wire.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "harness.hpp"

namespace raikiri::venues::lbank {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Lbank) {
    static const auto levels = std::make_unique<LevelBuffer>();
    static const auto overviews = std::make_unique<OverviewBuffer>();
    static const auto spot_levels = std::make_unique<SpotLevelBuffer>();
    static const auto contracts = std::make_unique<PerpInstrumentBuffer>();
    static const auto markets = std::make_unique<PerpMarketBuffer>();
    static const auto pairs = std::make_unique<SpotPairBuffer>();
    static const auto symbols = std::make_unique<SymbolBuffer>();
    static const auto depth = std::make_unique<RestDepthBuffer>();
    hammer("lbank",
           {
               {"ws_book_*",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_book(f, *levels);
                }},
               {"ws_trade.json",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_head(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_overview.json",
                [](std::string_view f) {
                    (void)parse_head(f);
                    (void)parse_overview(f, *overviews);
                }},
               {"ws_ack.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_refusal.json", [](std::string_view f) { (void)parse_head(f); }},
               {"ws_resume_note.json", [](std::string_view f) { (void)parse_head(f); }},
               {"rest_instrument.json", [](std::string_view f) { (void)parse_perp_instruments(f, *contracts); }},
               {"rest_market_data.json", [](std::string_view f) { (void)parse_perp_markets(f, *markets); }},
               {"rest_market_order.json", [](std::string_view f) { (void)parse_market_order(f, *depth); }},
               {"rest_accuracy.json", [](std::string_view f) { (void)parse_spot_pairs(f, *pairs); }},
               {"rest_ticker_24hr.json", [](std::string_view f) { (void)parse_spot_ticker_symbols(f, *symbols); }},
               {"spot_ws_*", [](std::string_view f) { (void)parse_spot_channel(f); }},
               {"spot_ws_depth.json", [](std::string_view f) { (void)parse_spot_depth(f, *spot_levels); }},
               {"spot_ws_trade.json", [](std::string_view f) { (void)parse_spot_trade(f); }},
               {"spot_ws_tick.json", [](std::string_view f) { (void)parse_spot_tick(f); }},
           });
}

} // namespace
} // namespace raikiri::venues::lbank
