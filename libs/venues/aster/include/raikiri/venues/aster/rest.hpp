#pragma once

// aster signs the query string exactly as sent for every method, post and delete included;
// reordering parameters after signing gets -1022

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::aster {

enum class BuildError : std::uint8_t {
    NoCredentials,
    BufferTooSmall,
    UnsupportedOrderType, // a market order with a price, or similar
    BadValue,             // a price or qty that will not render
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

// recvWindow is checked against the venue clock; pass the venue-corrected time as now
inline constexpr std::uint32_t k_recv_window_ms = 5'000;

class RequestBuilder {
public:
    explicit RequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    [[nodiscard]] net::RestRequest instruments() noexcept;
    // the funding interval lives only in fundingInfo
    [[nodiscard]] net::RestRequest funding_info() noexcept;
    [[nodiscard]] net::RestRequest premium_index() noexcept;
    [[nodiscard]] net::RestRequest ticker_24h() noexcept;
    [[nodiscard]] net::RestRequest server_time() noexcept;
    [[nodiscard]] net::RestRequest depth_snapshot(std::string_view symbol, std::uint32_t limit) noexcept;

    // exposed for tests: the exact bytes the last signed request signed
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    // the query is built into `query_`, signed, then joined with the path into `target_`
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view path, std::size_t query_len);

    [[nodiscard]] net::RestRequest header_only(net::HttpMethod method, std::string_view path) noexcept;

    std::array<char, 512> query_{};
    std::array<char, 768> target_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t sign_len_{0};
    const net::Signer* signer_;
};

} // namespace raikiri::venues::aster
