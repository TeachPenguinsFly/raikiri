#pragma once

// Binance USD-M futures REST request building.
//
// Signing rule, and it is the one that quietly breaks adapters: binance signs
// the QUERY STRING for every method, including POST and DELETE. There is no
// signed body — order parameters travel in the query even on a POST — and the
// signature covers the query EXACTLY as sent, in the exact order the
// parameters appear. Re-serialising the parameters after signing, or letting a
// map reorder them, produces a signature binance rejects as -1022, which reads
// like a bad key rather than a bad ordering.

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::binance {

enum class BuildError : std::uint8_t {
    NoCredentials,
    BufferTooSmall,
    UnsupportedOrderType, // a market order with a price, or similar
    BadValue,             // a price or qty that will not render
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

// Binance rejects a request whose timestamp is outside recvWindow of ITS
// clock. Five seconds is its own default and leaves room for a laptop that
// drifts; the venue-corrected clock is what should be passed in as `now`.
inline constexpr std::uint32_t k_recv_window_ms = 5'000;

class RequestBuilder {
public:
    explicit RequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    // --- unauthenticated ---
    [[nodiscard]] net::RestRequest instruments() noexcept;
    [[nodiscard]] net::RestRequest server_time() noexcept;
    [[nodiscard]] net::RestRequest depth_snapshot(std::string_view symbol,
                                                  std::uint32_t limit) noexcept;

    // --- signed ---
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(std::string_view symbol, const ExchangeClientOrderId& id, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    user_trades(std::string_view symbol, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> position_risk(UnixNanos now);

    // --- listen key (header auth only, never signed) ---
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_create();
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_keepalive();
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_close();

    // Exposed for tests: the exact bytes the last signed request signed.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    // Query is built into `query_`, signed, then joined with the path into `target_`.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    finish(net::HttpMethod method, std::string_view path, std::size_t query_len);

    [[nodiscard]] net::RestRequest header_only(net::HttpMethod method,
                                               std::string_view path) noexcept;

    std::array<char, 512> query_{};
    std::array<char, 768> target_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t sign_len_{0};
    const net::Signer* signer_;
};

} // namespace raikiri::venues::binance
