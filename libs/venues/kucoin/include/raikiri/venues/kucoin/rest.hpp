#pragma once

// KuCoin REST request building, unsigned and signed.
//
// SPOT AND FUTURES ARE TWO PRODUCTS behind one authentication scheme. They
// live on different hosts, spell their paths differently, and disagree about
// what a size is: spot sizes are base-currency decimals, futures sizes are
// whole CONTRACTS. Every builder here therefore takes the market and switches
// on it exhaustively rather than defaulting, so the day KuCoin lists options
// the compiler names each place that has to decide.
//
// There is no testnet: KuCoin retired its futures sandbox, so nothing here
// takes a flag.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::kucoin {

enum class BuildError : std::uint8_t {
    UnknownInstrument,  // not in the table: we never loaded it
    BadQuantity,        // does not render as a whole number of contracts / lots
    BadPrice,
    BadClientOrderId,   // absent, or longer than KuCoin's clientOid field
    UnsupportedMarket,  // options; KuCoin lists none on these APIs
    UnsupportedRequest, // the market has no such endpoint (spot amend aside)
    BufferTooSmall,     // payload exceeded the scratch buffer (a config bug)
    NotSigned,          // credentials were never loaded
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// The futures host, which is also the market-data host.
[[nodiscard]] std::string_view rest_host() noexcept;

// Empty for a market KuCoin does not serve, which is the caller's cue to
// refuse rather than to guess a host.
[[nodiscard]] std::string_view rest_host(MarketType market) noexcept;

// KuCoin's clientOid accepts letters, digits, '_' and '-'; the 26-char
// Crockford-base32 ULID the OMS mints is inside that alphabet and well inside
// the length. Pinned here so a widened id trips a test rather than a venue.
inline constexpr std::size_t k_max_client_oid = 40;

// Public reads. Futures host only — the market-data path is futures-only.
class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest bullet_public() noexcept;

    [[nodiscard]] net::RestRequest contracts_active() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest funding_rate_current(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest level2_snapshot(std::string_view symbol) noexcept;

    // Spot host. The catalogue is the one public read that is not futures.
    [[nodiscard]] net::RestRequest spot_symbols() noexcept;

private:
    std::array<char, 256> target_{};
};

// Key version 2 sends the passphrase HMAC'd under the api secret rather than
// in clear. A passphrase sent verbatim answers 400004 "passphrase error",
// which reads like the wrong passphrase and hides the real cause. The digest
// is constant for the life of the process, so it is sealed once, here.
class Credentials {
public:
    Credentials() = default;
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;

    [[nodiscard]] net::Signer& signer() noexcept { return signer_; }
    [[nodiscard]] const net::Signer& signer() const noexcept { return signer_; }

    // Must run AFTER the secret is loaded: the digest is keyed by it.
    [[nodiscard]] bool seal_passphrase(std::string_view raw) noexcept;

    [[nodiscard]] std::string_view sealed_passphrase() const noexcept {
        return {passphrase_.data(), passphrase_len_};
    }

    [[nodiscard]] bool complete() const noexcept {
        return signer_.has_credentials() && passphrase_len_ != 0;
    }

private:
    net::Signer signer_;
    std::array<char, net::k_base64_bytes> passphrase_{};
    std::size_t passphrase_len_{0};
};

// Signed requests. One builder owns one scratch buffer set, so a request must
// reach RestClient::send before the next one is built — which is exactly how
// the exec client uses it, one call at a time on the gateway thread.
class OrderRequestBuilder {
public:
    explicit OrderRequestBuilder(const Credentials& credentials) noexcept
        : credentials_{&credentials} {}

    OrderRequestBuilder(const OrderRequestBuilder&) = delete;
    OrderRequestBuilder& operator=(const OrderRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Spot only: KuCoin offers no modify on the futures API, so a perp amend
    // is refused here instead of being emulated by cancel+replace, which is
    // the OMS's decision to make and not the adapter's.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Futures answers account-wide; spot demands a symbol, which is why the
    // spot sweep goes through active_symbols() first.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> active_symbols(MarketType market,
                                                                             UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    fills(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // The private socket's handshake is a SIGNED post, unlike the public one.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> bullet_private(MarketType market,
                                                                             UnixNanos now);

    // Exposed for tests: the exact bytes that go into the HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view path, std::string_view body, UnixNanos now);

    const Credentials* credentials_;

    std::array<char, 1024> body_{};
    std::array<char, 256> path_{};
    std::array<char, 1536> sign_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::kucoin
