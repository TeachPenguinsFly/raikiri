#pragma once

// BitMEX v1 REST request building and signing.
//
// Signing is HMAC-SHA256, hex, over
//
//     verb + path + expires + body
//
// in api-key / api-signature / api-expires. Three details each fail as a 401
// that reads like a wrong secret:
//   * `path` INCLUDES the query string, exactly as it travels — so the
//     percent-encoding is decided before the signature, not by the transport;
//   * `expires` is an absolute wall-clock DEADLINE in seconds, not the request
//     time every other venue in this tree sends;
//   * a DELETE carries a body here, and that body is part of the digest.
//
// The catalogue is ONE request on purpose: the unauthenticated budget is
// per-minute and shared across the endpoint family (x-ratelimit-limit: 180).
// BitMEX publishes no server-time endpoint, so `expires` is local wall clock
// plus a window wide enough to absorb ordinary skew.

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

namespace raikiri::venues::bitmex {

[[nodiscard]] std::string_view rest_host(bool testnet) noexcept;

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: this adapter never loaded it
    BadQuantity,       // not a positive whole number of the venue's own units
    BadPrice,
    BufferTooSmall, // payload exceeded the scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NoOrderTarget,  // cancel/amend naming neither a venue id nor a client id
    EmptyBatch,
    BatchTooLarge,
    // BitMEX lists dated futures and this adapter can even price them, but
    // VenueConfig carries only enable_spot/enable_perp — so no dated row is
    // ever loaded, and an order for one must say THAT rather than blame the
    // catalogue for a market nobody asked it to fetch.
    DatedFuturesUnsupported,
    OptionsUnsupported,
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// Named before the instrument table is read by every builder that takes an
// OrderCmd. InstrumentTable::find compares the WHOLE id, so an id for a market
// this adapter never loaded resolves to nothing and would otherwise answer
// UnknownInstrument — a refusal that blames a stale catalogue when the truth is
// that the market is not served here.
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

class RequestBuilder {
public:
    // ~110 KiB, every listed contract in one answer, no market parameter.
    [[nodiscard]] net::RestRequest instruments() noexcept;

    // Single-row form, for confirming one contract without a catalogue pull.
    [[nodiscard]] net::RestRequest instrument(std::string_view symbol) noexcept;

    // NOT /orderBookL2_25: that is a websocket table name and 404s over REST.
    [[nodiscard]] net::RestRequest book(std::string_view symbol, std::uint32_t depth) noexcept;

    [[nodiscard]] net::RestRequest trades(std::string_view symbol, std::uint32_t count) noexcept;

private:
    std::array<char, 256> target_{};
};

class SignedRequestBuilder {
public:
    explicit SignedRequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    SignedRequestBuilder(const SignedRequestBuilder&) = delete;
    SignedRequestBuilder& operator=(const SignedRequestBuilder&) = delete;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // PUT /order takes a NEW TOTAL, not a delta, and `leavesQty` exists beside
    // `orderQty` for amending a partly filled order. This sends orderQty: the
    // OMS states the order's size, not what is left of it.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // One DELETE naming many ids. BitMEX keys a cancel on the order id alone,
    // so unlike most venues here a batch may straddle instruments.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    // GET /order?filter={"open":true} — every market at once. BitMEX offers no
    // market parameter, so the caller filters what comes back.
    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(std::uint32_t start,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    query_order(const ExchangeClientOrderId& id, UnixNanos now);

    // `start` is a row OFFSET, not a cursor: the window is pinned by
    // `since` and paged from the newest row backwards.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    fills(UnixNanos since, std::uint32_t start, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(UnixNanos now);

    // Exposed for tests: the exact bytes that went into the HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

    // "Send an array of ids" — the endpoint's own wording. 100 is BitMEX's
    // documented ceiling on a bulk order request.
    inline static constexpr std::size_t k_max_batch_cancel = 100;

    // BitMEX caps `count` at 500 on every table read.
    inline static constexpr std::uint32_t k_page_size = 500;

    // How far ahead of the local clock a request stays valid. Wide enough to
    // absorb ordinary skew on a venue that publishes no server time, narrow
    // enough that a captured request stops being replayable quickly.
    inline static constexpr std::uint64_t k_expiry_window_s = 30;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    sign(net::HttpMethod method, std::string_view path, std::string_view body, UnixNanos now);

    const net::Signer* signer_;

    // Scratch, reused per build: one request must be sent before the next is
    // built, which is the contract every builder in this repo carries. The
    // RestRequest hands out views into these.
    std::array<char, 4224> body_{};
    std::array<char, 4096> ids_{};
    std::array<char, 512> target_{};
    std::array<char, 4864> sign_msg_{};
    std::array<char, 24> expires_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::bitmex
