#pragma once

// HTX REST request building and signing.
//
// Signing: HMAC-SHA256 over
//
//     METHOD \n host \n path \n sorted-query
//
// base64-encoded, and it travels as QUERY PARAMETERS — AccessKeyId,
// SignatureMethod=HmacSHA256, SignatureVersion=2, Timestamp, Signature — never
// as headers. Three parts have no binance or bybit analogue and every one of
// them fails as err_code 403, which reads like a wrong secret:
//   * the query is sorted by parameter NAME in ASCII order, and what is signed
//     is the PERCENT-ENCODED pair list that is actually sent;
//   * the Timestamp is UTC at second precision, "2026-08-26T08:00:00", and its
//     colons are encoded INSIDE the signed string (%3A, uppercase hex);
//   * a POST signs only that query — the JSON body is NOT part of the digest.
//
// Spot and derivatives are two products on two hosts running two APIs
// (api.huobi.pro vs api.hbdm.com). The host is a term of the signature, so a
// request built for one and sent to the other authenticates as a bad key.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/htx/sign.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

// api.hbdm.com — derivatives, and the host the catalogue and market data use.
[[nodiscard]] std::string_view rest_host() noexcept;

// api.huobi.pro — spot.
[[nodiscard]] std::string_view spot_rest_host() noexcept;

[[nodiscard]] std::string_view rest_host(MarketType market) noexcept;

// This adapter trades USDT-margined swaps in CROSS margin. Isolated margin is
// a parallel endpoint family (swap_order / swap_openorders / swap_position_info
// with no `_cross`) and a parallel set of websocket topics; VenueConfig carries
// no margin-mode field, so widening to isolated means adding one and moving
// endpoints.hpp and ws.hpp with it, not switching this constant alone.
inline constexpr std::string_view k_margin_mode = "cross";

// The margin account cross mode settles in, and the `mar_acct` every ledger
// read names. Isolated margin spells this as the contract code instead, which
// is the same widening k_margin_mode above describes.
inline constexpr std::string_view k_cross_margin_account = "USDT";

// `type` codes 30 and 31 are funding income and funding expenditure. The rest
// of the table is fees, transfers and realised PnL — money this process either
// already booked from the fill or never touched.
inline constexpr std::string_view k_funding_record_types = "30,31";

// HTX requires lever_rate on every swap order and refuses one that disagrees
// with the leverage of an open position on the same contract. Nothing in
// OrderCmd or VenueConfig expresses leverage, so every order asks for this
// multiplier; an account holding positions at another one will see its orders
// refused rather than silently re-levered.
inline constexpr std::int32_t k_lever_rate = 1;

// Only meaningful in ONE-WAY (single-side) position mode, where HTX documents
// "both" as the sole legal value. In hedge mode `offset` must say open/close,
// which an OrderCmd does not express — such an account's orders are refused by
// the venue rather than opening the wrong leg.
inline constexpr std::string_view k_offset_one_way = "both";

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    BadQuantity,       // not a whole number of contracts, or not a legal size
    BadPrice,
    BufferTooSmall,          // payload exceeded the scratch buffer (a config bug)
    NotSigned,               // credentials were never loaded
    NoOrderTarget,           // cancel with neither a venue id nor a client id
    NoSpotAccount,           // spot order entry needs the account id from /v1/account/accounts
    ClientOrderIdNotNumeric, // see swap_client_id below
    // HTX prices a spot market BUY in QUOTE currency ("amount" is USDT for
    // btcusdt), and an OrderCmd carries base lots. There is no conversion that
    // is not a guess about the fill price, so the order is refused.
    SpotMarketBuyPricedInQuote,
    MixedBatch,         // one cancel batch straddling instruments
    BatchTooLarge,      // more ids than the endpoint accepts in one call
    SpotHasNoPositions, // a spot balance is not a position HTX can report
    SpotHasNoFunding,   // a spot pair pays none, and the swap ledger is not its
    // Neither API offers a modify: a live order is cancelled and replaced.
    AmendUnsupported,
    OptionsUnsupported,      // HTX lists options; this adapter does not trade them
    DatedFuturesUnsupported, // the dated-futures surface is a separate API
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// Named before the instrument table is read by every builder that takes an
// OrderCmd. InstrumentTable::find compares the WHOLE id, so an id for a market
// this adapter never loaded resolves to nothing and would otherwise answer
// UnknownInstrument — a refusal that blames a stale catalogue when the truth is
// that the market is not served here.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// HTX types the linear-swap client_order_id as an INT64 in
// [1, 9223372036854775807], so the 26-character ULID the OMS mints cannot
// travel on that market at all. The strategy's own ClientOrderId — already a
// u64, and OrderStore::route's PRIMARY key — goes instead, and comes back on
// every order push. Returns nullopt for an id that is zero or does not fit a
// positive int64 (a strategy_id at or above 0x8000 overflows the sign bit).
//
// Spot is unaffected: its client-order-id is a 64-character STRING over
// letters, digits, '_' and '-', which a Crockford ULID satisfies exactly.
[[nodiscard]] std::optional<std::int64_t> swap_client_id(ClientOrderId id) noexcept;

// The unauthenticated reads: two catalogues on two hosts, plus the clock.
class RequestBuilder {
public:
    [[nodiscard]] static net::RestRequest contracts() noexcept;

    [[nodiscard]] static net::RestRequest spot_symbols() noexcept;

    [[nodiscard]] static net::RestRequest server_time() noexcept;
};

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    // Learned once at start() from /v1/account/accounts; every spot order
    // carries it. Zero means "not learned", and spot order entry refuses.
    void set_spot_account_id(std::int64_t id) noexcept { spot_account_id_ = id; }

    [[nodiscard]] std::int64_t spot_account_id() const noexcept { return spot_account_id_; }

    [[nodiscard]] std::expected<net::RestRequest, BuildError> spot_accounts(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Always refuses. Neither HTX API can change a live order's price or size,
    // so an amend that looked like it worked would be a lie about the book; the
    // caller turns this into a ModifyRejected and the strategy re-quotes.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // Every command in the span must name the same instrument: the swap
    // endpoint takes contract_code beside the id list, so a mixed batch would
    // cancel against the wrong contract.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    // The swap sweep is paginated at 50 rows; `page` is 1-based.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::uint32_t page, UnixNanos now);

    // Spot resolves a client order id on its own. The swap read needs the
    // numeric id the venue was actually given and the contract it names, both
    // of which the caller looks up before asking.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id,
                std::int64_t numeric_id,
                std::string_view contract_code,
                MarketType market,
                UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // The account's funding rows, account-wide: `contract` is optional on this
    // endpoint, unlike on the trade history the fill pull cannot use.
    //
    // HTX refuses a window WIDER THAN 48 HOURS, so `since` is a floor and not
    // a promise — a cursor further back than that is clamped, and the rows
    // between are reachable only through the venue's own web download.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    financial_record(MarketType market, UnixNanos since, UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

    // "different IDs are separated by ',', maximum 10 orders can be withdrew at
    // one time" — the swap cancel's own wording. Spot's batchcancel takes 50.
    inline static constexpr std::size_t k_max_swap_batch_cancel = 10;
    inline static constexpr std::size_t k_max_spot_batch_cancel = 50;

    // page_size on swap_cross_openorders: "default 20, no more than 50".
    inline static constexpr std::uint32_t k_swap_page_size = 50;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError> sign(net::HttpMethod method,
                                                                   MarketType market,
                                                                   std::string_view path,
                                                                   sign::Query& query,
                                                                   std::string_view body,
                                                                   UnixNanos now);

    const net::Signer* signer_;
    std::int64_t spot_account_id_{0};

    // Scratch, reused per build: one request must be sent before the next is
    // built, which is the contract every builder in this repo carries.
    std::array<char, 2048> body_{};
    std::array<char, 192> path_{};
    std::array<char, 512> query_{};
    std::array<char, 832> target_{};
    std::array<char, 768> sign_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 96> encoded_signature_{};
    std::array<char, 24> timestamp_{};
    std::array<char, 32> account_text_{};
    std::array<char, 256> ids_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::htx
