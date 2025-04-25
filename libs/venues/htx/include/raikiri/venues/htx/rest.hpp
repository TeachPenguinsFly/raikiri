#pragma once

// HTX USDT-M swap REST request building.
//
// Only the unauthenticated reads the market-data path needs. Signing arrives
// with order entry, and its shape is already decided — HTX signs
// method + host + path + a SORTED query string with HMAC-SHA256 and carries
// AccessKeyId, SignatureMethod, SignatureVersion and an ISO-8601 Timestamp as
// QUERY parameters rather than headers. The sorted query is the part with no
// binance or bybit analogue, so recording it here keeps the next commit from
// rediscovering it.

#include <raikiri/net/rest_client.hpp>

#include <string_view>

namespace raikiri::venues::htx {

[[nodiscard]] std::string_view rest_host() noexcept;

// Both targets are constant, so unlike okx's builder this one carries no
// buffer; the class stays for the seam a parameterised endpoint will need.
class RequestBuilder {
public:
    // /linear-swap-api/v1/swap_contract_info — the whole USDT-M swap
    // catalogue in one call, roughly 115 KiB for 276 rows.
    [[nodiscard]] static net::RestRequest contracts() noexcept;

    // /api/v1/timestamp — the venue clock offset, measured before anything is
    // signed. It answers {"status":"ok","ts":<ms>} with no `data` at all.
    [[nodiscard]] static net::RestRequest server_time() noexcept;
};

} // namespace raikiri::venues::htx
