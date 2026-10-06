#pragma once

// every read is a post to /info naming the request in the body; content-type is required

#include <raikiri/net/rest_client.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace raikiri::venues::hyperliquid {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

[[nodiscard]] std::string_view info_path() noexcept;

[[nodiscard]] std::string_view exchange_path() noexcept;

// the address is public and travels in unsigned bodies
inline constexpr std::size_t k_address_chars = 42;

// constant bodies, so these requests may be held across each other
class RequestBuilder {
public:
    [[nodiscard]] static net::RestRequest meta() noexcept;

    // the request views `body`, which must outlive it
    [[nodiscard]] static net::RestRequest meta(std::string_view dex, std::string& body);

    [[nodiscard]] static net::RestRequest perp_dexs() noexcept;

    [[nodiscard]] static net::RestRequest perp_categories() noexcept;

    [[nodiscard]] static net::RestRequest meta_and_asset_ctxs() noexcept;

    // an empty dex is the main one; the request views `body`, which must outlive it
    [[nodiscard]] static net::RestRequest meta_and_asset_ctxs(std::string_view dex, std::string& body);

    // a different index space from meta.universe
    [[nodiscard]] static net::RestRequest spot_meta() noexcept;

    // every spot pair's context in one read
    [[nodiscard]] static net::RestRequest spot_meta_and_asset_ctxs() noexcept;
};

// one body buffer; a request is invalid once the next is built
class UserRequestBuilder {
public:
    [[nodiscard]] net::RestRequest clearinghouse_state(std::string_view user) noexcept;

    [[nodiscard]] net::RestRequest spot_state(std::string_view user) noexcept;

    // perp coins ("BTC") and spot coins ("@107") come in one answer
    [[nodiscard]] net::RestRequest open_orders(std::string_view user) noexcept;

    // the only read that says whether a resting order is post-only
    [[nodiscard]] net::RestRequest frontend_open_orders(std::string_view user) noexcept;

    // newest 2000 fills; `startTime` is inclusive, in ms
    [[nodiscard]] net::RestRequest user_fills(std::string_view user) noexcept;
    [[nodiscard]] net::RestRequest user_fills_by_time(std::string_view user, std::uint64_t start_ms) noexcept;

    // `startTime` is required and the answer capped at 500 rows, so the caller walks forward
    [[nodiscard]] net::RestRequest user_funding(std::string_view user, std::uint64_t start_ms) noexcept;

    [[nodiscard]] net::RestRequest order_status(std::string_view user, std::uint64_t oid) noexcept;
    [[nodiscard]] net::RestRequest order_status(std::string_view user, std::string_view cloid) noexcept;

private:
    [[nodiscard]] net::RestRequest simple(std::string_view type, std::string_view user) noexcept;

    std::array<char, 256> body_{};
};

} // namespace raikiri::venues::hyperliquid
