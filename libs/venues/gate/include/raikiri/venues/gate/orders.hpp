#pragma once

// An order's life on the wire: what the channel says happened to it, what a
// REST answer says about it, and the fills it produced.
//
// Spot and futures are two APIs wearing one brand. The same order is `amount`
// on one and `size` on the other, a decimal string here and a signed integer
// there, `status` on one transport and `event` on another. The normalisation
// happens here so the mapper sees one shape, and the two readers stay apart so
// neither one's quirks leak into the other.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// What an order frame says happened, normalised across the two APIs. Gate
// spells this three different ways — futures `finish_as` (with the WS-only
// pseudo-values `_new` and `_update`), spot `status`, and the spot socket's
// per-order `event` — and none of them is a superset of the others.
enum class WireOrderState : std::uint8_t {
    Open,      // accepted, still working
    Filled,    // finished, fully filled
    Cancelled, // finished by a cancel, a liquidation or self-trade prevention
    Expired,   // finished because ioc/poc/fok could not be met
    Unknown,
};

// One order across both APIs. `amount` and `left` are MAGNITUDES: futures
// signs both to carry the side, and a reader that keeps the sign turns a short
// into a negative quantity halfway down the mapper.
struct WireOrder {
    std::string_view contract;
    std::string_view order_id; // `id`, venue-minted
    std::string_view text;     // our "t-<ulid>" comes back here
    std::string_view price;
    std::string_view amount; // futures `size`, spot `amount`
    std::string_view left;
    std::string_view fill_price; // AVERAGE over the order, not this fill
    std::string_view tif;        // gtc, ioc, poc, fok
    std::string_view order_type; // spot only; futures has no type field
    bool sell{false};
    bool accepted{false}; // the frame that says the order reached the book
    WireOrderState state{WireOrderState::Unknown};
    std::uint64_t ts_ms{0};
};

// Gate's own cap is 1000 rows per list call, which is also the largest
// websocket batch observed.
inline constexpr std::size_t k_max_private_entries = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_private_entries> orders{};
};

// One fill. There is no `left` here on either API — the remaining size lives
// on the ORDERS channel — so a mapper needs both frames to state leaves.
struct WireUserTrade {
    std::string_view contract;
    std::string_view trade_id; // `id`, the dedupe key
    std::string_view order_id;
    std::string_view text;
    std::string_view price;
    std::string_view qty; // magnitude of futures `size` / spot `amount`
    std::string_view fee;
    bool sell{false};
    bool maker{false};
    std::uint64_t ts_ms{0};
};

struct UserTradeBuffer {
    std::array<WireUserTrade, k_max_private_entries> trades{};
};

// Websocket pushes: the rows hang off `result`, an array on both channels.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_ws_orders(std::string_view frame, OrderBuffer& storage, MarketType market);

[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError>
parse_ws_user_trades(std::string_view frame, UserTradeBuffer& storage, MarketType market);

// REST bodies: a top-level array, with no envelope to unwrap.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market);

[[nodiscard]] std::expected<WireOrder, WireError> parse_rest_order(std::string_view body,
                                                                   MarketType market);

// GET /spot/open_orders answers with one row PER CURRENCY PAIR, each holding
// its own `orders` array. Reading it as a flat order list yields nothing at
// all, silently.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_spot_open_orders(std::string_view body, OrderBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError>
parse_rest_user_trades(std::string_view body, UserTradeBuffer& storage, MarketType market);

} // namespace raikiri::venues::gate
