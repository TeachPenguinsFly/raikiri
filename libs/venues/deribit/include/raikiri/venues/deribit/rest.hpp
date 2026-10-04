#pragma once

// json-rpc over rest: the method is the path, params the query, each request signed by header.
// query values are already url-safe, so nothing is percent-encoded

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/deribit/sign.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

// usdc series are linear and the mapper refuses them
inline constexpr std::array<std::string_view, 2> k_currencies{"BTC", "ETH"};

// the perpetual and dated future share kind future
[[nodiscard]] std::string_view kind_of(MarketType market) noexcept;

// settlement coin from the instrument name; empty when the name has no '-'
[[nodiscard]] std::string_view currency_of(std::string_view instrument_name) noexcept;

// deribit caps label at 64 chars
inline constexpr std::size_t k_max_label = 64;
static_assert(k_max_label >= sizeof(ExchangeClientOrderId::data), "Deribit `label` must carry a whole 26-char ULID");

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,       // not a whole number of amount steps, or not positive
    BadPrice,
    BufferTooSmall, // the request outgrew its scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel/amend with neither a venue id nor a client id
    UnknownCurrency,
    SpotNotModelled,
    NothingToAmend,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// checked before the instrument lookup so an unserved market is not reported as unknown
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest instruments(std::string_view currency, std::string_view kind) noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

private:
    std::array<char, 256> target_{};
};

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    // amount is in the named instrument's unit, which lot_size already holds
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // cancel by venue id when known, else by label
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // edit_by_label works only while one open order carries the label
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view currency, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    order_state_by_label(const ExchangeClientOrderId& id, std::string_view currency, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    user_trades(MarketType market, std::string_view currency, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    positions(MarketType market, std::string_view currency, UnixNanos now);

    // settlement entries only, newest first; a zero continuation asks for the first page
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_settlements(std::string_view currency, UnixNanos since, std::uint64_t continuation, UnixNanos now);

    [[nodiscard]] std::string_view last_sign_message() const noexcept;

    inline static constexpr std::uint32_t k_max_trade_page = 1000;
    inline static constexpr std::uint32_t k_max_log_page = 1000;
    // how far back a first funding pull reads, with no cursor yet
    inline static constexpr std::uint64_t k_first_funding_window_ns = 7ULL * 86'400ULL * 1'000'000'000ULL;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign(std::string_view target, UnixNanos now);

    const net::Signer* signer_;
    std::uint64_t nonce_counter_{0};

    // scratch reused per build: send a request before building the next
    std::array<char, 768> target_{};
    std::array<char, 1024> sign_msg_{};
    std::array<char, 24> nonce_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::array<char, sign::k_max_auth_header> header_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::deribit
