#pragma once

// Hyperliquid /info and /exchange request building.
//
// The shape every other adapter assumes — a GET whose target names the resource
// — does not exist here. Every read is a POST to the single /info path whose
// BODY names the request, {"type":"meta"}, and every write is a POST to
// /exchange. There are no per-action paths at all. RestRequest already carries
// method, target, body and content_type, so libs/net needed no change. Both
// VERIFIED live and easy to get wrong: omitting Content-Type earns 415 rather
// than being ignored (RestRequest defaults it), and a GET here earns 405.
//
// Signing is not modelled here: order entry is an EIP-712 signature over an
// msgpack-encoded action, sharing nothing with the CEX builders' HMAC over a
// query string. It lives in sign.hpp and exchange.hpp.

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::hyperliquid {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

[[nodiscard]] std::string_view info_path() noexcept;

[[nodiscard]] std::string_view exchange_path() noexcept;

// "0x" + 40 hex. Every account-scoped read names one, and it is NOT secret:
// it travels in a plain unsigned body, which is why every read path below
// works with no credentials beyond knowing the address.
inline constexpr std::size_t k_address_chars = 42;

// Bodies for the unparameterised reads are compile-time constants. Nothing
// writes into a scratch buffer, so unlike the CEX builders these requests may
// be held across each other.
class RequestBuilder {
public:
    // The perp catalogue: {"universe":[{"name":"BTC","szDecimals":5,...},...]}.
    [[nodiscard]] static net::RestRequest meta() noexcept;

    [[nodiscard]] static net::RestRequest meta_and_asset_ctxs() noexcept;

    // The spot catalogue: {"universe":[pairs],"tokens":[...]}. A DIFFERENT
    // index space from meta.universe — see asset_index() in exchange.hpp.
    [[nodiscard]] static net::RestRequest spot_meta() noexcept;
};

// The account-scoped reads. One body buffer, so a request is dead the moment
// the next one is built — hold the response, never the request.
class UserRequestBuilder {
public:
    // Perp positions and margin. Spot has no equivalent: see spot_state.
    [[nodiscard]] net::RestRequest clearinghouse_state(std::string_view user) noexcept;

    // Spot BALANCES, which are not positions. Kept so the refusal in
    // position_reports can be about what the venue models rather than a guess.
    [[nodiscard]] net::RestRequest spot_state(std::string_view user) noexcept;

    // Both markets in one answer: `coin` is "BTC" for a perp and "@107" for a
    // spot pair, so the caller filters rather than asking twice.
    [[nodiscard]] net::RestRequest open_orders(std::string_view user) noexcept;

    // Same rows plus orderType, tif, reduceOnly and the trigger fields — the
    // only read that says whether a resting order is post-only.
    [[nodiscard]] net::RestRequest frontend_open_orders(std::string_view user) noexcept;

    // Newest 2000 fills, newest first. `startTime` is INCLUSIVE and in
    // milliseconds.
    [[nodiscard]] net::RestRequest user_fills(std::string_view user) noexcept;
    [[nodiscard]] net::RestRequest user_fills_by_time(std::string_view user,
                                                      std::uint64_t start_ms) noexcept;

    // By venue order id. There is a by-cloid form too, but a cloid the venue
    // never saw and one it has forgotten answer identically, so the id we were
    // given back is the one worth asking about.
    [[nodiscard]] net::RestRequest order_status(std::string_view user,
                                                std::uint64_t oid) noexcept;
    [[nodiscard]] net::RestRequest order_status(std::string_view user,
                                                std::string_view cloid) noexcept;

private:
    [[nodiscard]] net::RestRequest simple(std::string_view type, std::string_view user) noexcept;

    // Longest body is orderStatus with a 34-char cloid; 256 is slack, not a
    // measurement.
    std::array<char, 256> body_{};
};

} // namespace raikiri::venues::hyperliquid
