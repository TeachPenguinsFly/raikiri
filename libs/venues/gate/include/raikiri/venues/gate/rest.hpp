#pragma once

// Gate.io v4 REST request building.
//
// Signing (VERIFIED against gateapi-python's gen_sign and live against
// api.gateio.ws, 2026-08): the digest is HMAC-SHA512, hex lowercase, over
//
//     METHOD \n /api/v4/<path> \n <query> \n SHA512(body) \n <seconds>
//
// and travels in KEY / SIGN / Timestamp headers. Three parts have no binance
// or okx analogue and each one fails as a generic "Invalid key provided":
// SHA512 rather than SHA256, the BODY appears only as its own hash, and the
// signed path carries the /api/v4 prefix that the request line also carries.
// The Timestamp is SECONDS — a millisecond value is answered REQUEST_EXPIRED
// (verified live), and the tolerance is 60 s either way.
//
// The query string is signed URL-DECODED. Every parameter this builder emits
// is already unreserved ASCII, so encoded and decoded forms coincide; a
// parameter that needs escaping would have to be signed in its decoded form.
//
// Testnet is a different HOST here (api-testnet.gateapi.io, verified
// answering), not a header the way okx does it.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

// USDT-settled perpetuals. Gate keys the whole futures API on the settlement
// currency, so this reaches every path rather than being a query parameter.
// Widening it means widening map_instrument's ContractKind in the same change:
// /futures/btc/contracts returns INVERSE contracts through the same parser.
inline constexpr std::string_view k_settle_usdt = "usdt";

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest contracts(std::string_view settle) noexcept;
    [[nodiscard]] net::RestRequest tickers(std::string_view settle) noexcept;

    // GET /spot/currency_pairs — the spot half of the catalogue.
    [[nodiscard]] net::RestRequest currency_pairs() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest
    order_book(std::string_view settle, std::string_view contract, std::uint32_t depth) noexcept;

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
    // Gate spot prices a MARKET BUY in QUOTE currency ("amount" is USDT for
    // BTC_USDT), and an OrderCmd carries base lots. There is no conversion
    // that is not a guess about the fill price, so the order is refused.
    SpotMarketBuyNotExpressible,
    OptionsUnsupported,      // Gate lists options; this adapter does not trade them
    DatedFuturesUnsupported, // Gate's /delivery API is a separate surface
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// Gate's client order id. The venue requires the `t-` prefix and allows 28
// bytes AFTER it (0-9 A-Z a-z _ - .), so a 26-char Crockford ULID fits with
// two bytes to spare and needs no escaping. k_client_id_bytes is the whole
// field including the prefix.
inline constexpr std::string_view k_client_id_prefix = "t-";
inline constexpr std::size_t k_client_id_body_max = 28;
inline constexpr std::size_t k_client_id_bytes = k_client_id_prefix.size() + k_client_id_body_max;

static_assert(sizeof(ExchangeClientOrderId::data) <= k_client_id_body_max,
              "gate rejects a client order id longer than 28 bytes after the t- prefix");

// Renders "t-<ulid>" into caller storage; empty when the id would not fit.
[[nodiscard]] std::string_view client_id_text(const ExchangeClientOrderId& id,
                                              std::span<char> out) noexcept;

// Strips the `t-` prefix Gate echoes back. Anything without the prefix is one
// of Gate's own reserved sources (`api`, `web`, `liq-...`) and is NOT ours —
// attributing it to an order would match a fill to the wrong id.
[[nodiscard]] std::string_view client_id_from_text(std::string_view text) noexcept;

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Futures takes a bare array of order ids; spot pairs every id with its
    // currency_pair. Both cap at 20 per call, so callers batch.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    // Perp only: spot's single-order read needs the currency_pair the caller
    // does not have, so the spot path answers from the open-order sweep.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id, MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // Funding settlements from the account book. Paired with
    // `VenueCaps::funding_charge`.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_book(MarketType market, UnixNanos since, UnixNanos now);

    // GET /futures/{settle}/accounts, whose `user` is the id the futures
    // private socket demands as the first element of every subscribe payload.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> futures_account(UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

    inline static constexpr std::size_t k_max_batch_cancel = 20;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign(net::HttpMethod method,
                                                                   std::string_view path,
                                                                   std::string_view query,
                                                                   std::string_view body,
                                                                   UnixNanos now);

    const net::Signer* signer_;

    // Scratch, reused per build: one request must be sent before the next is
    // built, which is the same contract the unsigned builder above carries.
    std::array<char, 2048> body_{};
    std::array<char, 256> path_{};
    std::array<char, 256> query_{};
    std::array<char, 320> target_{};
    std::array<char, 640> sign_msg_{};
    std::array<char, net::k_hex512_bytes> body_hash_{};
    std::array<char, net::k_hex512_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::gate
