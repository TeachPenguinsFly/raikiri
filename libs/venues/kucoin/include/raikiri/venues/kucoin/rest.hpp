#pragma once

// spot and futures differ in host, path and size unit; builders switch on the market exhaustively

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
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,       // does not render as a whole number of contracts / lots
    BadPrice,
    BadClientOrderId,   // absent, or longer than KuCoin's clientOid field
    UnsupportedMarket,  // options; KuCoin lists none on these APIs
    UnsupportedRequest, // the market has no such endpoint (spot amend aside)
    BufferTooSmall,     // payload exceeded the scratch buffer (a config bug)
    NotSigned,          // credentials were never loaded
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

[[nodiscard]] std::string_view rest_host() noexcept;

// empty for a market kucoin does not serve
[[nodiscard]] std::string_view rest_host(MarketType market) noexcept;

// the futures wallet defaults to XBT when currency is omitted; widening this means widening map_instrument's FFWCSX
// guard
inline constexpr std::string_view k_settle_currency = "USDT";

// a few days under three months, the widest funding-history window
inline constexpr std::uint64_t k_funding_window_ns = 89ULL * 86'400ULL * 1'000'000'000ULL;

// clientOid length limit; the oms's 26-char ulid fits
inline constexpr std::size_t k_max_client_oid = 40;

// clientOid alphabet: letters, digits, '_' and '-'
[[nodiscard]] bool client_oid_usable(std::string_view id) noexcept;

// public reads, futures host only
class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest bullet_public() noexcept;

    [[nodiscard]] net::RestRequest contracts_active() noexcept;
    [[nodiscard]] net::RestRequest spot_all_tickers() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest funding_rate_current(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest level2_snapshot(std::string_view symbol) noexcept;

    // spot host
    [[nodiscard]] net::RestRequest spot_symbols() noexcept;

private:
    std::array<char, 256> target_{};
};

// key version 2 sends the passphrase hmac'd under the secret; sent verbatim it answers 400004
class Credentials {
public:
    Credentials() = default;
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;

    [[nodiscard]] net::Signer& signer() noexcept { return signer_; }

    [[nodiscard]] const net::Signer& signer() const noexcept { return signer_; }

    // run after the secret is loaded; the digest is keyed by it
    [[nodiscard]] bool seal_passphrase(std::string_view raw) noexcept;

    [[nodiscard]] std::string_view sealed_passphrase() const noexcept { return {passphrase_.data(), passphrase_len_}; }

    [[nodiscard]] bool complete() const noexcept { return signer_.has_credentials() && passphrase_len_ != 0; }

private:
    net::Signer signer_;
    std::array<char, net::k_base64_bytes> passphrase_{};
    std::size_t passphrase_len_{0};
};

// one scratch buffer set: send each request before building the next
class OrderRequestBuilder {
public:
    explicit OrderRequestBuilder(const Credentials& credentials) noexcept : credentials_{&credentials} {}

    OrderRequestBuilder(const OrderRequestBuilder&) = delete;
    OrderRequestBuilder& operator=(const OrderRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // spot only; futures has no modify endpoint
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // futures answers account-wide; spot needs a symbol, see active_symbols()
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> active_symbols(MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> fills(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    // futures only: positions closed since `since`, newest first
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    closed_positions(UnixNanos since, std::uint64_t page, UnixNanos now);

    // one symbol per call; a zero offset asks for the first page
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_history(std::string_view symbol, UnixNanos since, std::uint64_t offset, UnixNanos now);

    // futures wallet only; the spot accounts list is a different shape
    [[nodiscard]] std::expected<net::RestRequest, BuildError> account_overview(MarketType market, UnixNanos now);

    // the private handshake is a signed post
    [[nodiscard]] std::expected<net::RestRequest, BuildError> bullet_private(MarketType market, UnixNanos now);

    // exposed for tests: the exact bytes that go into the hmac
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
