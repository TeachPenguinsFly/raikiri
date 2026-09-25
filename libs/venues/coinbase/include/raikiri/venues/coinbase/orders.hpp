#pragma once

// no private stream: every row comes from rest

#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// the same row from POST /orders, GET /orders/{id} and GET /orders; no leaves field
struct WireOrder {
    std::string_view id;         // uuid, the venue's own handle
    std::string_view client_oid; // the UUID derived from our ULID; see rest.hpp
    std::string_view product_id;
    std::string_view status;        // open, pending, active, rejected, done
    std::string_view done_reason;   // filled, canceled; only on a done order
    std::string_view reject_reason; // why the engine refused it
    std::string_view price;         // absent on a market order
    std::string_view size;
    std::string_view filled_size;
    std::string_view created_at; // ISO-8601
    std::string_view done_at;
    bool sell{false};
};

// GET /orders caps a page at 1000; this asks for the cap
inline constexpr std::size_t k_max_orders = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// POST /orders and GET /orders/{id} answer with the bare object
[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view body);

// GET /orders answers with an array
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                                OrderBuffer& storage);

} // namespace raikiri::venues::coinbase
