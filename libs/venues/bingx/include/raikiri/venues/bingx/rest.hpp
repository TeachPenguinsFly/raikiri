#pragma once

// BingX REST request building — ONE HOST, TWO API TREES, ONE SIGNATURE.
//
// Spot answers under /openApi/spot/v1 and perpetuals under /openApi/swap/v2,
// and the two disagree about almost everything a request says: the field that
// carries our own order id, the HTTP verb a cancel uses, whether an answer
// wraps the order in `data` or in `data.order`. What they DO share is the host
// and the signature, so unlike xt there is one signer here and no second clock
// to keep.
//
// The signature (VERIFIED against BingX's authentication reference and against
// ccxt's live-tested implementation):
//
//   canonical = every parameter, `timestamp` included, as `k=v` joined by '&'
//   signature = HMAC-SHA256(secret, canonical), lowercase hex
//   request   = METHOD path?canonical&signature=<hex>, X-BX-APIKEY header
//
// Two properties of that are easy to get wrong and both present as a revoked
// key rather than as a build bug. First, the digest covers the RAW parameter
// string — BingX percent-encodes values on the wire only when one contains
// '[' or '{', and signs the unencoded form either way; this builder sends no
// such value and refuses any that would need encoding, so the string signed
// and the string sent are the same bytes out of the same buffer. Second,
// `recvWindow` is an ordinary parameter: it participates in the digest exactly
// like the rest, so sending it unsigned or signing it unsent both fail.
//
// The parameters are written in ASCII order because that is the documented
// convention, but the invariant the server actually enforces is the stronger
// one above: it verifies the query string it RECEIVED, in the order it
// received it. Building the two from one buffer is what makes that true.
//
// POST and DELETE carry their parameters in the query string, not in a body —
// BingX's own examples and ccxt both do this, and the body stays empty.

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

    // GET /openApi/spot/v1/common/symbols — the spot half of the catalogue.
    // Unlike the swap contracts it states a tickSize and a stepSize outright,
    // so nothing here is derived from a digit count.
    [[nodiscard]] net::RestRequest spot_symbols() noexcept;

    // One host serves both trees, so one offset is right for both.
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
    // A parameter value carrying '&', '=', '?', '#' or a newline. BingX signs
    // the query string verbatim, so such a value would let one field's text
    // become two signed fields — the parameter-injection hole ccxt guards the
    // same way.
    UnsafeParameter,
    // BingX amends the QUANTITY of a resting order and nothing else. An amend
    // that restates a price is refused rather than sent as a size change that
    // silently leaves the price where it was.
    AmendPriceUnsupported,
    SpotAmendUnsupported,
    SpotPositionsUnsupported,
    OptionsUnsupported,
    DatedFuturesUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// The markets this adapter serves, named exhaustively so Option is a case the
// compiler forces a decision about rather than something a default arm
// swallows. Called BEFORE the instrument table is read: the table resolves an
// id by its WHOLE value, so an id for a market this adapter never loaded finds
// nothing and would otherwise answer UnknownInstrument — a refusal that blames
// a stale catalogue when the truth is the market is not served here.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// GTC/IOC/FOK/PostOnly. Post-only is a time in force here, not a flag beside
// one, and both trees spell it the same way.
[[nodiscard]] std::string_view time_in_force(TimeInForce tif, OrderFlags flags) noexcept;

// BOTH in one-way mode, LONG/SHORT in hedge mode — and in hedge mode it is NOT
// the direction: a SELL that closes a long carries positionSide LONG. Reading
// it as the direction closes nothing and opens the opposite position instead.
[[nodiscard]] std::string_view position_side(Side side, OrderFlags flags, bool hedge) noexcept;

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    // Read once from /openApi/swap/v1/positionSide/dual. The two modes need
    // DIFFERENT parameters — hedge wants LONG/SHORT and refuses reduceOnly,
    // one-way wants BOTH and accepts it — and BingX answers a mismatch with
    // 109400 rather than opening the position the caller did not ask for.
    void set_hedge_mode(bool on) noexcept { hedge_ = on; }

    [[nodiscard]] bool hedge_mode() const noexcept { return hedge_; }

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Perpetual only, and QUANTITY only: /openApi/swap/v1/trade/amend takes no
    // price, and spot offers no amend at all — only a cancelReplace that mints
    // a new order under a new id.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Account-wide on both trees: `symbol` is optional and omitting it is what
    // makes this a sweep rather than a per-pair sample.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> position_mode(UnixNanos now);

    // The margin wallet. Perpetual only and takes no market: the spot tree
    // answers a different shape on a different path, and the caller refuses
    // there rather than reading the wrong wallet under a spot label.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> balance(UnixNanos now);

    // Funding settlements from the account's own book. No incomeType filter:
    // the same call states TRADING_FEE beside FUNDING_FEE, and the fee rows
    // are what a fee reconciliation compares against.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> funding_income(UnixNanos since,
                                                                             UnixNanos now);

    // POST mints, PUT extends by an hour, DELETE closes. BingX documents these
    // as signed and its own curl example sends no signature; signing is the
    // strict superset, so they are signed here.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_create(UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    listen_key_keepalive(std::string_view key, UnixNanos now);
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key_close(std::string_view key,
                                                                               UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC, which
    // are also the exact bytes of the query string that was sent.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view path, std::string_view canonical);

    const net::Signer* signer_;
    bool hedge_{false};

    // Scratch, reused per build: a request must be sent before the next one is
    // built. One order at a time is the only use.
    std::array<char, 512> query_{};
    std::array<char, 768> target_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t canonical_len_{0};
};

} // namespace raikiri::venues::bingx
