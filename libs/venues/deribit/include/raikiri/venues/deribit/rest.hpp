#pragma once

// Deribit v2 REST request building and signing.
//
// Everything is JSON-RPC 2.0 even over REST: the METHOD is the path and the
// params are the query string, while the reply is a JSON-RPC envelope rather
// than the bare payload — which is why one reply parser serves both
// transports. Private methods are GET requests too: there is no body to sign
// on any call this adapter makes.
//
// Authentication is the stateless `deri-hmac-sha256` header (see sign.hpp),
// NOT the bearer token public/auth mints. Both are offered; the header signs
// each request on its own, so nothing here has to notice a token expiring
// mid-flight, and every builder below is testable without credentials.
//
// Percent-encoding is deliberately absent. Every value this file puts in a
// query is already URL-safe: instrument names come from Deribit's own
// catalogue (letters, digits, '-' and '_'), the label is a Crockford base32
// ULID, order ids are "<CCY>-<digits>", and the numbers are rendered by
// format_e9. A value outside that set would be a schema change, not an
// escaping bug.

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

// The coin-margined complex, which is the whole reason this venue is here:
// USDC-settled series are linear and the mapper refuses them, so pulling them
// would be bytes spent to be discarded.
inline constexpr std::array<std::string_view, 2> k_currencies{"BTC", "ETH"};

// Deribit's own word for the group an instrument belongs to. The perpetual and
// the dated future share "future" — there is no "perpetual" kind, and asking
// for one is an Invalid params error (verified live).
[[nodiscard]] std::string_view kind_of(MarketType market) noexcept;

// The settlement coin, read off the instrument name: "BTC-PERPETUAL" and
// "BTC-11AUG26-64000-C" are both BTC. Every currency-scoped private method
// needs it and no OrderCmd carries it. Empty when the name has no '-', which
// no reversed instrument the mapper accepts ever lacks.
[[nodiscard]] std::string_view currency_of(std::string_view instrument_name) noexcept;

// `label` is Deribit's client-order-id field, capped at 64 characters — twice
// what the OMS mints, so the ULID travels whole. The check below is not
// ceremony: a venue that shortened this would otherwise get a silently
// truncated id, and two orders sharing a prefix would answer one cancel.
inline constexpr std::size_t k_max_label = 64;
static_assert(k_max_label >= sizeof(ExchangeClientOrderId::data),
              "Deribit `label` must carry a whole 26-char ULID");

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,       // not a whole number of amount steps, or not positive
    BadPrice,
    BufferTooSmall, // the request outgrew its scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel/amend with neither a venue id nor a client id
    UnknownCurrency,
    // Deribit lists spot, and the catalogue mapper refuses every spot row, so
    // an order for one names an instrument this process has never loaded.
    SpotNotModelled,
    // An amend that changes neither price nor quantity. Deribit's edit takes
    // both as optional but needs one of them, and a request carrying neither
    // spends a round trip on a command that was a no-op to begin with.
    NothingToAmend,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// Named before the instrument table is read by every builder that takes an
// OrderCmd. InstrumentTable::find compares the WHOLE id, so an id for a market
// this adapter never loaded resolves to nothing and would otherwise answer
// UnknownInstrument — a refusal that blames a stale catalogue when the truth is
// that the market is not served here.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// The unauthenticated reads.
class RequestBuilder {
public:
    [[nodiscard]] net::RestRequest instruments(std::string_view currency,
                                               std::string_view kind) noexcept;

    // Measures the venue clock offset.
    [[nodiscard]] net::RestRequest server_time() noexcept;

private:
    std::array<char, 256> target_{};
};

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    // private/buy or private/sell. One shape serves the inverse perpetual, the
    // dated future and the option: Deribit reads `amount` in the unit the
    // NAMED instrument is sized in, which is exactly what lot_size already
    // holds, so the option path needs no separate arithmetic and gets none.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // private/cancel when the order store has the venue id, private/cancel_by_
    // label otherwise. The label is a unique ULID, so cancelling "all orders
    // with this label" cancels exactly one.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // private/edit, or private/edit_by_label when only the ULID is known —
    // which Deribit honours only while exactly one open order carries it, a
    // condition a per-order ULID satisfies by construction.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::string_view currency, UnixNanos now);

    // Resolves the OMS's own id without any local bookkeeping: the ULID went
    // out as the label and comes back as one.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    order_state_by_label(const ExchangeClientOrderId& id,
                         std::string_view currency,
                         UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    user_trades(MarketType market, std::string_view currency, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    positions(MarketType market, std::string_view currency, UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

    // count on get_user_trades_by_currency; the venue's own ceiling.
    inline static constexpr std::uint32_t k_max_trade_page = 1000;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign(std::string_view target,
                                                                   UnixNanos now);

    const net::Signer* signer_;
    std::uint64_t nonce_counter_{0};

    // Scratch, reused per build: one request must be sent before the next is
    // built, which is the contract every builder in this repo carries.
    std::array<char, 768> target_{};
    std::array<char, 1024> sign_msg_{};
    std::array<char, 24> nonce_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::array<char, sign::k_max_auth_header> header_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::deribit
