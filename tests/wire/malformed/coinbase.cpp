#include <raikiri/venues/coinbase/book.hpp>
#include <raikiri/venues/coinbase/catalogue.hpp>
#include <raikiri/venues/coinbase/envelope.hpp>
#include <raikiri/venues/coinbase/market_data.hpp>
#include <raikiri/venues/coinbase/orders.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::venues::coinbase {
namespace {

using wire::malformed::hammer;

TEST(Malformed, Coinbase) {
    hammer("coinbase",
           {
               {"rest_market_products.json",
                [](std::string_view f) {
                    static ProductVolumeBuffer storage;
                    (void)parse_product_volumes(f, storage);
                }},
               {"ws_ticker.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_ticker(f);
                }},
               {"ws_*match*",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_match(f);
                }},
               {"ws_error.json",
                [](std::string_view f) {
                    (void)parse_event(f);
                    (void)parse_stream_error(f);
                }},
               {"ws_subscriptions.json",
                [](std::string_view f) {
                    static AckBuffer acks;
                    (void)parse_event(f);
                    (void)parse_subscriptions(f, acks);
                }},
               {"ws_subscriptions_level2_renamed.json",
                [](std::string_view f) {
                    static AckBuffer acks;
                    (void)parse_subscriptions(f, acks);
                }},
               {"ws_subscriptions_empty.json",
                [](std::string_view f) {
                    static AckBuffer acks;
                    (void)parse_subscriptions(f, acks);
                }},
               {"ws_l2_snapshot.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_event(f);
                    (void)parse_l2_snapshot(f, levels);
                }},
               {"ws_l2update.json",
                [](std::string_view f) {
                    static ChangeBuffer changes;
                    (void)parse_event(f);
                    (void)parse_l2_update(f, changes);
                }},
               {"ws_l2update_chained.json",
                [](std::string_view f) {
                    static ChangeBuffer changes;
                    (void)parse_l2_update(f, changes);
                }},
               {"rest_book_level1.json",
                [](std::string_view f) {
                    static LevelBuffer levels;
                    (void)parse_l2_snapshot(f, levels);
                }},
               {"rest_products.json",
                [](std::string_view f) {
                    static ProductBuffer products;
                    (void)parse_products(f, products);
                }},
               {"rest_product_btc.json", [](std::string_view f) { (void)parse_product(f); }},
               {"rest_time.json", [](std::string_view f) { (void)parse_server_time(f); }},
               {"rest_error_*", [](std::string_view f) { (void)parse_rest_error(f); }},
               {"rest_order_*", [](std::string_view f) { (void)parse_order(f); }},
               {"rest_orders_open.json",
                [](std::string_view f) {
                    static OrderBuffer orders;
                    (void)parse_orders(f, orders);
                }},
           });
}

} // namespace
} // namespace raikiri::venues::coinbase
