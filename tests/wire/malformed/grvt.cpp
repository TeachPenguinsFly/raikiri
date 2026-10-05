#include <raikiri/venues/grvt/wire.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::grvt {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Grvt) {
    hammer("grvt",
           {
               {"ws_book_d_ack.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_book_d_snapshot.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_book_d_delta.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_book_d_delta_next.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_mini_d_snapshot.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_mini_d_delta.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_trade.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_trade_snapshot.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_ticker_s.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"ws_error.json",
                [](std::string_view f) {
                    static MessageBuffer storage;
                    (void)parse_message(f, storage);
                }},
               {"rest_error.json", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"rest_all_instruments*",
                [](std::string_view f) {
                    static InstrumentBuffer storage;
                    (void)parse_instruments(f, storage);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::grvt
