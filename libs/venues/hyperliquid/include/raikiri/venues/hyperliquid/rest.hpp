#pragma once

// Hyperliquid /info request building.
//
// The shape every other adapter assumes — a GET whose target names the resource
// — does not exist here. Every market-data read is a POST to the one /info path
// whose BODY names the request, {"type":"meta"}. RestRequest already carries
// method, target, body and content_type, so libs/net needed no change. Both
// VERIFIED live and easy to get wrong: omitting Content-Type earns 415 rather
// than being ignored (RestRequest defaults it), and a GET here earns 405.
//
// Signing is not modelled: order entry is an EIP-712 signature over an action
// struct, sharing nothing with the CEX builders' HMAC over a query string.

#include <raikiri/net/rest_client.hpp>

#include <string_view>

namespace raikiri::venues::hyperliquid {

[[nodiscard]] std::string_view rest_host() noexcept;

[[nodiscard]] std::string_view info_path() noexcept;

// Bodies are compile-time constants because none of these reads is
// parameterised. Nothing writes into a scratch buffer, so unlike the CEX
// builders these requests may be held across each other.
class RequestBuilder {
public:
    // The catalogue: {"universe":[{"name":"BTC","szDecimals":5,...},...]}.
    [[nodiscard]] static net::RestRequest meta() noexcept;

    // Catalogue plus live per-asset context. Not used by the adapter — the
    // activeAssetCtx channel pushes the identical objects, on change — but it
    // is the only REST form of that data, and it feeds the same parser.
    [[nodiscard]] static net::RestRequest meta_and_asset_ctxs() noexcept;
};

} // namespace raikiri::venues::hyperliquid
