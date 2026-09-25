#pragma once

// hmac-sha256 over the raw query string, sent verbatim with the X-BX-APIKEY header
// recvWindow is signed like any parameter; post and delete carry parameters in the query

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace raikiri::venues::bingx {

[[nodiscard]] std::string_view rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contracts() noexcept;

    // states tickSize and stepSize outright
    [[nodiscard]] net::RestRequest spot_symbols() noexcept;

    // one host serves both trees, so one clock offset
    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest premium_index(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

private:
    std::array<char, 256> target_{};
};

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,
    BadPrice,
    BufferTooSmall,
    NotSigned,     // credentials were never loaded
    NoOrderTarget, // cancel/amend with neither a venue id nor a usable ULID
    // bingx signs the query verbatim, so these would inject a parameter
    UnsafeParameter,
    // bingx amends quantity only
    AmendPriceUnsupported,
    SpotAmendUnsupported,
    SpotPositionsUnsupported,
    OptionsUnsupported,
    DatedFuturesUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// checked before the instrument table so an unserved market is not reported as unknown
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// post-only is a time in force here
[[nodiscard]] std::string_view time_in_force(TimeInForce tif, OrderFlags flags) noexcept;

// in hedge mode this names the position side; a sell that closes a long sends long
[[nodiscard]] std::string_view position_side(Side side, OrderFlags flags, bool hedge) noexcept;

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    // hedge mode wants long/short and refuses reduceOnly; one-way wants both
    void set_hedge_mode(bool on) noexcept { hedge_ = on; }

    [[nodiscard]] bool hedge_mode() const noexcept { return hedge_; }

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // perp only and quantity only; spot has no amend
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // symbol omitted, so this sweeps the account
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> position_mode(UnixNanos now);

    // perp only; spot has a different wallet shape
    [[nodiscard]] std::expected<net::RestRequest, BuildError> balance(UnixNanos now);

    // no incomeType filter, so fee rows come back too
    [[nodiscard]] std::expected<net::RestRequest, BuildError> funding_income(UnixNanos since, UnixNanos now);

    // post mints, put extends by an hour, delete closes; signed although bingx's curl example is not
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_create(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_keepalive(std::string_view key, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_close(std::string_view key, UnixNanos now);

    // for tests: the bytes last signed, identical to the query sent
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view path, std::string_view canonical);

    const net::Signer* signer_;
    bool hedge_{false};

    // reused per build; send a request before building the next
    std::array<char, 512> query_{};
    std::array<char, 768> target_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t canonical_len_{0};
};

} // namespace raikiri::venues::bingx
