#pragma once

// futures and spot share api.mexc.com but sign differently: spot signs
// the query, futures apiKey + Request-Time + params

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// contract.mexc.com, public futures market data only
[[nodiscard]] std::string_view rest_host() noexcept;

// api.mexc.com, every signed call
[[nodiscard]] std::string_view private_rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contract_detail() noexcept;

    // GET /api/v3/exchangeInfo, the spot catalogue
    [[nodiscard]] net::RestRequest exchange_info() noexcept;
    [[nodiscard]] net::RestRequest spot_ticker_24h() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    // spot signs against its own clock
    [[nodiscard]] net::RestRequest spot_server_time() noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;
    [[nodiscard]] net::RestRequest funding_rates() noexcept;

private:
    std::array<char, 256> target_{};
};

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,       // quantity does not render as a venue-legal size
    BadPrice,
    BufferTooSmall, // payload exceeded the scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel/amend with neither a venue id nor a client id
    // spot has no time-in-force and lists no ioc or fok types, so those are refused
    SpotTifNotExpressible,
    AmendUnsupported,        // neither MEXC API offers a modify
    OptionsUnsupported,      // MEXC lists no options on either API
    DatedFuturesUnsupported, // /contract serves perpetuals only
    NeedsSymbol,             // spot reads are per-symbol; the caller must name one
    SpotBatchCancelUnsupported,
    SpotPositionsUnsupported,
    SpotFundingUnsupported, // a spot pair pays none, and there is no ledger of one
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// checked before the table lookup, so an unserved market is not reported as UnknownInstrument
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// 1 open long, 2 close short, 3 open short, 4 close long; a buy is 1 or 2 depending on reduce-only
[[nodiscard]] std::int64_t futures_side(Side side, OrderFlags flags) noexcept;

// 1 limit, 2 post-only, 3 ioc, 4 fok, 5 market, 6 market-on-close
[[nodiscard]] std::int64_t
futures_order_type(OrderType type, TimeInForce tif, OrderFlags flags) noexcept;

[[nodiscard]] std::string_view spot_order_type(OrderType type, OrderFlags flags) noexcept;

// externalOid caps at 32 bytes (error 2030)
inline constexpr std::size_t k_client_id_max = 32;

static_assert(sizeof(ExchangeClientOrderId::data) <= k_client_id_max,
              "mexc refuses an externalOid longer than 32 bytes");

// futures cancel caps the id array; callers chunk
inline constexpr std::size_t k_max_batch_cancel = 20;

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // mexc has no modify on either market
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // futures takes an array of ids; spot has no batch cancel by id
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    // `symbol` is required on spot and optional on futures
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> query_order(
        const ExchangeClientOrderId& id, MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, std::string_view symbol, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // futures only; spot pays no funding
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_records(MarketType market, UnixNanos since, UnixNanos now);

    // for tests: the bytes of the last hmac
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    // signs `query` verbatim, so nothing may reorder it afterwards
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign_spot(net::HttpMethod method, std::string_view path, std::string_view query, UnixNanos now);

    // the query must already be sorted by parameter name
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign_futures(net::HttpMethod method,
                                                                           std::string_view path,
                                                                           std::string_view query,
                                                                           std::string_view body,
                                                                           UnixNanos now);

    const net::Signer* signer_;

    // reused per build; send a request before building the next
    std::array<char, 1024> body_{};
    std::array<char, 512> query_{};
    std::array<char, 256> path_{};
    std::array<char, 768> target_{};
    std::array<char, 1280> sign_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::mexc
