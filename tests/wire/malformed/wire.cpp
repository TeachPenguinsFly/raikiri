#include <raikiri/wire/json.hpp>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace raikiri::wire {
namespace {

using malformed::hammer;

void walk(ondemand::value& value, int depth);

void walk_array(ondemand::array& array, int depth) {
    for (auto element : array) {
        ondemand::value item;
        if (element.get(item) != simdjson::SUCCESS) {
            return;
        }
        walk(item, depth + 1);
    }
}

void walk(ondemand::value& value, int depth) {
    if (depth > 8) {
        return;
    }
    ondemand::json_type type{};
    if (value.type().get(type) != simdjson::SUCCESS) {
        return;
    }
    switch (type) {
    case ondemand::json_type::object: {
        ondemand::object obj;
        if (value.get_object().get(obj) == simdjson::SUCCESS) {
            (void)for_each_field(
                obj, [&](std::string_view, ondemand::value& child) { walk(child, depth + 1); });
        }
        return;
    }
    case ondemand::json_type::array: {
        ondemand::array array;
        if (value.get_array().get(array) == simdjson::SUCCESS) {
            walk_array(array, depth);
        }
        return;
    }
    case ondemand::json_type::string:
        (void)str_of(value);
        return;
    case ondemand::json_type::number:
        (void)u64_of(value);
        return;
    case ondemand::json_type::boolean:
        (void)bool_of(value);
        return;
    default:
        (void)raw_of(value);
        return;
    }
}

void in_order(std::string_view frame) {
    auto root = session().open_object(frame);
    if (!root.has_value()) {
        auto array = session().open_array(frame);
        if (array.has_value()) {
            walk_array(*array, 0);
        }
        return;
    }
    (void)for_each_field(*root, [](std::string_view, ondemand::value& child) { walk(child, 0); });
}

// out of wire order on purpose: every lookup after the first rescans
void by_name(ondemand::object& obj) {
    if (auto levels = req_array(obj, "asks"); levels.has_value()) {
        walk_array(*levels, 0);
    }
    (void)opt_u64(obj, "t");
    (void)opt_str(obj, "instrument_name");
    if (auto levels = req_array(obj, "bids"); levels.has_value()) {
        walk_array(*levels, 0);
    }
    (void)opt_i64(obj, "u");
    (void)opt_bool(obj, "ok");
    (void)opt_ms(obj, "ts");
    (void)opt_raw(obj, "data");
    (void)has_field(obj, "missing");
    if (auto inner = req_object(obj, "update"); inner.has_value()) {
        by_name(*inner);
    }
    (void)req_str(obj, "channel");
}

void lookups(std::string_view frame) {
    auto root = session().open_object(frame);
    if (root.has_value()) {
        by_name(*root);
        if (auto result = req_object(*root, "result"); result.has_value()) {
            if (auto data = req_array(*result, "data"); data.has_value()) {
                for (auto element : *data) {
                    ondemand::object row;
                    if (element.get_object().get(row) != simdjson::SUCCESS) {
                        break;
                    }
                    by_name(row);
                }
            }
        }
        return;
    }
    auto array = session().open_array(frame);
    if (!array.has_value()) {
        return;
    }
    for (auto element : *array) {
        ondemand::object row;
        if (element.get_object().get(row) != simdjson::SUCCESS) {
            return;
        }
        by_name(row);
    }
}

TEST(Malformed, WireHelpers) {
    for (const auto venue : {"cryptocom"}) {
        hammer(venue,
               {
                   {"fuzz_book_trailing_comma.json", in_order},
                   {"fuzz_book_trailing_comma.json", lookups},
                   {"fuzz_book_update_unclosed.json", in_order},
                   {"fuzz_book_update_unclosed.json", lookups},
                   {"ws_book_perp.json", in_order},
                   {"ws_book_perp.json", lookups},
                   {"ws_book_update_perp.json", lookups},
               });
    }
}

} // namespace
} // namespace raikiri::wire
