#pragma once

// spot and futures order shapes differ; normalised here so the mapper sees one

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// normalises futures finish_as, spot status and the spot socket's event
enum class WireOrderState : std::uint8_t {
    Open,      // accepted, still working
    Filled,    // finished, fully filled
    Cancelled, // finished by a cancel, a liquidation or self-trade prevention
    Expired,   // finished because ioc/poc/fok could not be met
    Unknown,
};

// amount and left are magnitudes; futures signs both
struct WireOrder {
    std::string_view contract;
    std::string_view order_id; // `id`, venue-minted
    std::string_view text;     // our "t-<ulid>" comes back here
    std::string_view price;
    std::string_view amount; // futures `size`, spot `amount`
    std::string_view left;
    std::string_view fill_price; // average over the order
    std::string_view tif;        // gtc, ioc, poc, fok
    std::string_view order_type; // spot only; futures has no type field
    bool sell{false};
    bool accepted{false}; // the frame that says the order reached the book
    WireOrderState state{WireOrderState::Unknown};
    std::uint64_t ts_ms{0};
};

// gate caps a list call at 1000 rows
inline constexpr std::size_t k_max_private_entries = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_private_entries> orders{};
};

// no left here; leaves come from the orders channel
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

// websocket pushes: rows hang off result
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_ws_orders(std::string_view frame, OrderBuffer& storage, MarketType market);

[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError>
parse_ws_user_trades(std::string_view frame, UserTradeBuffer& storage, MarketType market);

// rest bodies: a top-level array
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market);

[[nodiscard]] std::expected<WireOrder, WireError> parse_rest_order(std::string_view body, MarketType market);

// one row per currency pair, each with its own orders array
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_spot_open_orders(std::string_view body,
                                                                                          OrderBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireUserTrade>, WireError>
parse_rest_user_trades(std::string_view body, UserTradeBuffer& storage, MarketType market);

} // namespace raikiri::venues::gate
