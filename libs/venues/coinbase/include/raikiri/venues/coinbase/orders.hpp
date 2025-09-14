#pragma once

// An order's life on the wire. There is no private stream on this adapter:
// every row below comes back from REST, and one reader serves all three
// endpoints that answer it.

#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// One order row, identical from POST /orders, GET /orders/{id} and each
// element of GET /orders. EVERY numeric field is a decimal STRING here — this
// venue never sends a bare JSON number for money — so nothing below needs the
// lenient number reading Deribit's mixed spellings force.
//
// There is no leaves field: `size` minus `filled_size` is it, and both are in
// BASE currency whatever the side.
struct WireOrder {
    std::string_view id;         // uuid, the venue's own handle
    std::string_view client_oid; // the UUID derived from our ULID; see rest.hpp
    std::string_view product_id;
    std::string_view status;        // open, pending, active, rejected, done
    std::string_view done_reason;   // filled, canceled — only on a done order
    std::string_view reject_reason; // why the engine refused it
    std::string_view price;         // absent on a market order
    std::string_view size;
    std::string_view filled_size;
    std::string_view created_at; // ISO-8601
    std::string_view done_at;
    bool sell{false};
};

// GET /orders caps a page at 1000 and this adapter asks for the cap.
inline constexpr std::size_t k_max_orders = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// POST /orders and GET /orders/{id} answer with the bare object.
[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view body);

// GET /orders answers with an array.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

} // namespace raikiri::venues::coinbase
