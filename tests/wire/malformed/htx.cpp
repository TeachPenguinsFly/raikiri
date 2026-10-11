#include <raikiri/net/inflater.hpp>
#include <raikiri/venues/htx/account.hpp>
#include <raikiri/venues/htx/book.hpp>
#include <raikiri/venues/htx/catalogue.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/market_data.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::htx {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Htx) {
    hammer("htx",
           {
               {"*.frame.gz",
                [](std::string_view f) {
                    static net::Inflater inflater;
                    (void)parse_event(f);
                    if (const auto frame = inflater.inflate(f); frame.has_value()) {
                        (void)parse_event(*frame);
                    }
                }},
               {"ws_bbo.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_depth_step0*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_depth(f, levels);
                }},
               {"ws_trade*",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_event(f);
                    (void)parse_trades(f, trades);
                }},
               {"ws_detail.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_detail(f);
                }},
               {"rest_swap_index.json",
                [](std::string_view f) {
                    static OpenInterestBuffer storage;
                    (void)parse_swap_index(f, storage);
                }},
               {"rest_swap_open_interest.json",
                [](std::string_view f) {
                    static OpenInterestBuffer storage;
                    (void)parse_open_interest(f, storage);
                }},
               {"ws_notify_funding.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_funding(f);
                }},
               {"ws_*error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_*ping.json", [](std::string_view f) { (void)parse_event(f); }},
               {"ws_*sub*_ack.json", [](std::string_view f) { (void)parse_event(f); }},
               {"rest_contracts.json",
                [](std::string_view f) {
                    static ContractBuffer storage;
                    (void)parse_rest_envelope(f);
                    (void)parse_contracts(f, storage);
                }},
               {"ws_spot_bbo.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_bbo(f);
                }},
               {"ws_spot_depth*",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_depth(f, levels);
                }},
               {"ws_spot_trade*",
                [](std::string_view f) {
                    static TradeBuffer trades;
                    (void)parse_trades(f, trades);
                }},
               {"rest_spot_symbols.json",
                [](std::string_view f) {
                    static SpotSymbolBuffer storage;
                    (void)parse_rest_envelope(f);
                    (void)parse_spot_symbols(f, storage);
                }},
               {"rest_spot_tickers.json",
                [](std::string_view f) {
                    static SpotTickerBuffer storage;
                    (void)parse_spot_tickers(f, storage);
                }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_envelope(f); }},
               {"rest_server_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_swap_financial_record.json",
                [](std::string_view f) {
                    static FinancialRecordBuffer storage;
                    (void)parse_financial_records(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::htx
