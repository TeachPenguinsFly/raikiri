#pragma once

// MEXC REST request building — two different exchanges wearing one brand.
//
// ONE HOST, TWO AUTHENTICATION SCHEMES. Futures moved off contract.mexc.com
// onto api.mexc.com, so /api/v1 (futures) and /api/v3 (spot) now answer from
// the same name — and they still sign nothing alike:
//
//   spot    /api/v3   every parameter, POST included, travels in the QUERY
//                     STRING; the HMAC-SHA256 hex digest covers that query
//                     exactly as sent plus recvWindow and timestamp; the key
//                     rides in X-MEXC-APIKEY and the digest is appended to
//                     the query as &signature=.
//   futures /api/v1   digest covers apiKey + Request-Time + (sorted query for
//                     GET/DELETE | raw JSON body for POST), travels in a
//                     Signature header, and path parameters are EXCLUDED.
//
// Sharing one signer between them is the bug that reads as a revoked key.
//
// VERIFIED 2026-08-26 against production: contract.mexc.com answers a private
// GET with MEXC's own {"success":false,"code":401} but 403s a private POST at
// the CDN, while api.mexc.com answers both with {"code":402,...}. Order entry
// therefore goes to api.mexc.com; the public futures market data the data
// client reads still comes off contract.mexc.com, which serves it unchanged.
//
// A signed spot request MISSING its timestamp/signature is answered
// {"code":404,"msg":"Not Found"} rather than a parameter error (VERIFIED
// 2026-08-26: GET /api/v3/openOrders?symbol=BTCUSDT with a key header and
// nothing else), so a build bug here looks like a routing bug. That is the
// second reason the signed paths are separate functions with their bytes
// pinned by tests.

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

// contract.mexc.com — public futures market data, and the only host the data
// client talks to.
[[nodiscard]] std::string_view rest_host() noexcept;

// api.mexc.com — every SIGNED call, spot and futures alike. One host does not
// mean one scheme: see the file header.
[[nodiscard]] std::string_view private_rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contract_detail() noexcept;

    // GET /api/v3/exchangeInfo — the spot half of the catalogue.
    [[nodiscard]] net::RestRequest exchange_info() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    // GET /api/v3/time. Spot signs against ITS clock, and the two hosts answer
    // from separate deployments.
    [[nodiscard]] net::RestRequest spot_server_time() noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t limit) noexcept;

    [[nodiscard]] net::RestRequest ticker(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;

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
    // MEXC spot has no timeInForce parameter; the tif would have to ride in
    // `type`, and although the reference lists IMMEDIATE_OR_CANCEL and
    // FILL_OR_KILL as enum values, NOT ONE of the 2123 exchangeInfo rows on
    // 2026-08-26 advertised either — every row offered LIMIT/MARKET/
    // LIMIT_MAKER and 35 of them only the two limit forms. An IOC or FOK spot
    // order is therefore refused rather than downgraded to GTC, which would
    // leave a resting order where the strategy asked for fill-or-nothing.
    SpotTifNotExpressible,
    AmendUnsupported,        // neither MEXC API offers a modify
    OptionsUnsupported,      // MEXC lists no options on either API
    DatedFuturesUnsupported, // /contract serves perpetuals only
    NeedsSymbol,             // spot reads are per-symbol; the caller must name one
    SpotBatchCancelUnsupported,
    SpotPositionsUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// The markets this adapter serves, named exhaustively so Option is a case the
// compiler forces a decision about rather than something a default arm
// swallows. Called BEFORE the instrument table is read: the table resolves an
// id by its whole value, so an id for a market this adapter never loaded finds
// nothing and would otherwise answer UnknownInstrument — a refusal that blames
// a stale catalogue when the truth is the market is not served here.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// MEXC's futures `side` is a four-valued OPEN/CLOSE enum, not a direction:
// 1 open long, 2 close short, 3 open short, 4 close long. Buy therefore maps
// to 1 or 2 depending on whether the order reduces, and getting that wrong
// does not fail — it opens the opposite position.
[[nodiscard]] std::int64_t futures_side(Side side, OrderFlags flags) noexcept;

// 1 limit, 2 post-only maker, 3 IOC, 4 FOK, 5 market, 6 market-on-close.
// Post-only displaces the time in force here the way it does on bybit.
[[nodiscard]] std::int64_t
futures_order_type(OrderType type, TimeInForce tif, OrderFlags flags) noexcept;

// LIMIT / MARKET / LIMIT_MAKER — the three MEXC spot advertises.
[[nodiscard]] std::string_view spot_order_type(OrderType type, OrderFlags flags) noexcept;

// externalOid is capped at 32 bytes (error 2030 past it) and a 26-char ULID
// fits with room to spare.
inline constexpr std::size_t k_client_id_max = 32;

static_assert(sizeof(ExchangeClientOrderId::data) <= k_client_id_max,
              "mexc refuses an externalOid longer than 32 bytes");

// Futures cancel takes a JSON ARRAY of order ids and caps the call, so callers
// chunk rather than truncate.
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

    // Refused on both markets: MEXC offers no modify anywhere, and a
    // cancel-replace synthesized here would hide the loss of queue position.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Futures takes a bare array of venue order ids. Spot has no batch cancel
    // by id at all — only cancel-everything-on-a-symbol — so a spot batch is
    // refused here and the caller falls back to one call per order.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    // `symbol` is mandatory on spot (MEXC's openOrders has no account-wide
    // form) and optional on futures, where empty means every contract.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> query_order(
        const ExchangeClientOrderId& id, MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, std::string_view symbol, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    // Spot: signs `query` verbatim and appends &signature=. The digest covers
    // the query in the order the caller wrote it, so nothing may re-order it
    // afterwards.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign_spot(net::HttpMethod method, std::string_view path, std::string_view query, UnixNanos now);

    // Futures: signs apiKey + Request-Time + (query | body) and sends the
    // digest in a header. The query MUST already be sorted by parameter name.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign_futures(net::HttpMethod method,
                                                                           std::string_view path,
                                                                           std::string_view query,
                                                                           std::string_view body,
                                                                           UnixNanos now);

    const net::Signer* signer_;

    // Scratch, reused per build: a request must be sent before the next is
    // built, which is the same contract the unsigned builder above carries.
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
