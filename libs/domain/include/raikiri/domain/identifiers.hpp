#pragma once

// Identifier scheme.

#include <raikiri/common/fixed_string.hpp>
#include <raikiri/common/ulid.hpp>

#include <compare>
#include <cstdint>
#include <string_view>

namespace raikiri {

enum class VenueId : std::uint8_t {
    Invalid = 0,
    Binance,
    Bybit,
    Okx,
    Gate,
    Kucoin,
    Bingx,
    Xt,
    Mexc,
    Sim,
    Deribit,
    Hyperliquid,
    Coinbase,
    Kraken,
    Bitmex,
    Bitget,
    Htx,
    Bitfinex,
    Weex,
    Aster,
};

inline constexpr std::size_t k_venue_count = 20; // including Invalid

enum class MarketType : std::uint8_t {
    Spot = 0,
    Perp,
    Future,
    Option,
};

enum class Side : std::uint8_t { Buy = 0, Sell };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] std::string_view to_string(VenueId venue) noexcept;
[[nodiscard]] VenueId venue_from_string(std::string_view name) noexcept; // Invalid if unknown
[[nodiscard]] std::string_view to_string(MarketType market) noexcept;
[[nodiscard]] std::string_view to_string(Side side) noexcept;

// Dense per-venue symbol index, interned at instrument load (bounded: one per
// listed instrument). The text form lives in the instrument table only.
struct SymbolId {
    std::uint32_t raw{0};

    friend constexpr auto operator<=>(SymbolId, SymbolId) noexcept = default;
};

// {venue:8 | market:8 | reserved:16 | symbol:32} — one u64, trivially
// hashable, and the (venue, market) prefix is the routing key.
struct InstrumentId {
    std::uint64_t packed{0};

    [[nodiscard]] static constexpr InstrumentId
    make(VenueId venue, MarketType market, SymbolId symbol) noexcept {
        return {(static_cast<std::uint64_t>(venue) << 56) |
                (static_cast<std::uint64_t>(market) << 48) | symbol.raw};
    }

    [[nodiscard]] constexpr VenueId venue() const noexcept {
        return static_cast<VenueId>(packed >> 56);
    }

    [[nodiscard]] constexpr MarketType market() const noexcept {
        return static_cast<MarketType>((packed >> 48) & 0xFFU);
    }

    [[nodiscard]] constexpr SymbolId symbol() const noexcept {
        return {static_cast<std::uint32_t>(packed & 0xFFFF'FFFFU)};
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return venue() != VenueId::Invalid; }

    friend constexpr auto operator<=>(InstrumentId, InstrumentId) noexcept = default;
};

// One trading account per (venue, market) pair. It is the routing key for
// everything a process owns per market — its shm segment, its journal, its
// credentials: perp = "bybit", spot = "bybit-spot".
struct AccountId {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Perp};

    friend constexpr auto operator<=>(AccountId, AccountId) noexcept = default;
};

// Formats into caller storage ("bybit", "bybit-spot", "bybit-fut",
// "bybit-opt"); buf must hold >= 16 bytes.
[[nodiscard]] std::string_view
format_account(AccountId account, char* buf, std::size_t buf_len) noexcept;

// The inverse. Invalid venue when the name does not parse, so a caller that
// forgets to check gets a refusal rather than venue 0 wearing a market.
[[nodiscard]] AccountId account_from_string(std::string_view name) noexcept;

// Strategy-minted order handle: {strategy_id:16 | seq:48}.
struct ClientOrderId {
    std::uint64_t raw{0};

    [[nodiscard]] static constexpr ClientOrderId make(std::uint16_t strategy_id,
                                                      std::uint64_t seq) noexcept {
        return {(static_cast<std::uint64_t>(strategy_id) << 48) | (seq & 0xFFFF'FFFF'FFFFULL)};
    }

    [[nodiscard]] constexpr std::uint16_t strategy_id() const noexcept {
        return static_cast<std::uint16_t>(raw >> 48);
    }

    [[nodiscard]] constexpr std::uint64_t seq() const noexcept { return raw & 0xFFFF'FFFF'FFFFULL; }

    [[nodiscard]] constexpr bool valid() const noexcept { return raw != 0; }

    friend constexpr auto operator<=>(ClientOrderId, ClientOrderId) noexcept = default;
};

// The manual desk's strategy id, reserved and never handed to a strategy.
//
// A hand-placed order still needs a client order id, and every one of those
// packs the strategy that owns it. Reserving one value means an order a person
// placed is distinguishable from a strategy's in the book, the journal and the
// ledger — and it means strategyd can refuse an intent that claims to be
// somebody else, because the only id a desk may mint is this one.
inline constexpr std::uint16_t k_manual_strategy_id = 1;

// OMS-minted, sent to the venue as its client-order-id field
// (orderLinkId / clientOid / newClientOrderId depending on venue).
using ExchangeClientOrderId = Ulid;

// Venue-minted, unbounded cardinality — inline text, never interned.
using ExchangeOrderId = FixedString<40>;
using ExecId = FixedString<40>;

} // namespace raikiri

template <> struct std::hash<raikiri::InstrumentId> {
    [[nodiscard]] std::size_t operator()(raikiri::InstrumentId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.packed);
    }
};

template <> struct std::hash<raikiri::ClientOrderId> {
    [[nodiscard]] std::size_t operator()(raikiri::ClientOrderId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.raw);
    }
};
