#pragma once

// XT REST request building — TWO EXCHANGES, TWO HOSTS, TWO SIGNATURES.
//
// Futures answers from fapi.xt.com and spot from sapi.xt.com, and the digest
// is built differently on each. Both sign `X + Y`, both send the result in
// `validate-signature`, and there the resemblance stops:
//
//   spot     X = the four validate-* headers as `k=v` sorted alphabetically
//                and joined with `&`
//            Y = `#METHOD#path` plus `#query` and `#body` when present
//   futures  X = validate-appkey and validate-timestamp ONLY — no recvwindow,
//                no algorithms
//            Y = `#path` plus `#params`. The HTTP METHOD IS NOT SIGNED.
//
// Three traps, each of which presents as a revoked key rather than as a build
// bug: sharing one signer between the trees, carrying the method into the
// futures digest, and sending a `xt-validate-*` header while signing a
// `validate-*` one (the server rebuilds X out of the headers it received, so
// the prefix on the wire and the prefix in the digest must match).
//
// VERIFIED against XT's own Go SDK (xt-com/xt-go-demo, xt/v4/{spot,future}/
// utils.go) and against six self-computed vectors pinned in the venue tests.
// XT's own published example signature does NOT reproduce — that doc page
// quotes two different appkeys — so the SDK is the reference, not the docs.

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

namespace raikiri::venues::xt {

// fapi.xt.com — futures, public market data and private order entry alike.
[[nodiscard]] std::string_view rest_host() noexcept;

// sapi.xt.com — the whole spot tree. Nothing is shared with the futures host,
// not even the envelope: spot answers {"rc":..,"mc":..} and futures
// {"returnCode":..,"error":{..}}.
[[nodiscard]] std::string_view spot_rest_host() noexcept;

class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest symbol_list() noexcept;

    // GET /v4/public/symbol — the spot half of the catalogue.
    [[nodiscard]] net::RestRequest spot_symbol_list() noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    // Spot signs against ITS clock: the two hosts answer from separate
    // deployments and one offset for both expires on whichever drifted.
    [[nodiscard]] net::RestRequest spot_server_time() noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view symbol) noexcept;

    [[nodiscard]] net::RestRequest depth(std::string_view symbol, std::uint32_t level) noexcept;

private:
    std::array<char, 256> target_{};
};

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,
    BadPrice,
    // XT counts futures size in CONTRACTS and refuses a fractional one with
    // invalid_quantity_scale. A size that does not land on a whole contract is
    // refused here rather than rounded into a position of a different size.
    FractionalContracts,
    BufferTooSmall,
    NotSigned,     // credentials were never loaded
    NoOrderTarget, // cancel/amend with no venue order id
    // The OMS mints 26-character ULIDs and XT's clientOrderId is
    // ^[a-zA-Z0-9_]{4,22}$. client_id.hpp re-encodes one losslessly into 22
    // characters; an id that is not a ULID has no such encoding and is refused
    // rather than sent in a shape the venue would echo back as something else.
    ClientIdNotEncodable,
    // A spot MARKET BUY is priced in QUOTE currency (`quoteQty`) and XT
    // refuses `quantity` on one. OrderCmd carries a base size and no quote
    // budget, so the order has nothing to spend and is refused rather than
    // converted at a price this builder would have to invent.
    MarketBuyNeedsQuoteQty,
    OptionsUnsupported,
    DatedFuturesUnsupported,
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

// LONG/SHORT, and it is NOT the direction: a SELL that closes a long carries
// positionSide LONG. Reading it as the direction closes nothing and opens the
// opposite position instead.
[[nodiscard]] std::string_view position_side(Side side, OrderFlags flags) noexcept;

// GTC/IOC/FOK/GTX. Post-only is a time in force here, not a flag beside one:
// GTX is the field's own value for it.
[[nodiscard]] std::string_view time_in_force(TimeInForce tif, OrderFlags flags) noexcept;

// Futures pages cap at 100 rows per read.
inline constexpr std::uint32_t k_page_size = 100;

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Both trees cancel by VENUE order id only. Futures has no single
    // cancel-by-client-id endpoint at all — only the batch form takes a
    // `clientOrderIds` list — and spot puts the order id in the PATH. A cancel
    // that knows only our own id is therefore refused rather than sent to an
    // endpoint that would silently cancel nothing.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Native on both: futures POST .../order/update, spot PUT /v4/order/{id}.
    // Both take a new price and a new quantity together, so an amend that
    // changes one still restates the other.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // `symbol` empty means every contract on futures and every pair on spot.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view symbol, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id, MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    my_trades(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // GET /future/user/v1/user/listen-key — valid 8 hours.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> listen_key(UnixNanos now);

    // POST /v4/ws-token — one call per 10 s, and the token lasts 2 days.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> ws_token(UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign_spot(net::HttpMethod method,
                                                                        std::string_view path,
                                                                        std::string_view query,
                                                                        std::string_view body,
                                                                        UnixNanos now);

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
    std::array<char, 2048> sign_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::xt
