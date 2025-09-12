#pragma once

// OKX v5 REST request building.
//
// There is no separate demo host: OKX switches an account to demo trading
// with an x-simulated-trading header on the same host. Public reads answer
// identically with or without it (verified), so the public builders take no
// flag and only the signed ones carry it.
//
// The signing asymmetry worth memorising: OKX counts a GET's query string as
// part of requestPath, '?' INCLUDED — `/api/v5/trade/fills?instType=SWAP` is
// signed whole — where bybit signs the query with the '?' stripped and the
// path left out entirely. Getting it wrong yields 50113 "Invalid signature",
// which reads exactly like a bad key.

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::venues::okx {

enum class BuildError : std::uint8_t {
    UnknownInstrument, // not in the table: we never loaded it
    UnsupportedMarket, // options; see the note on td_mode_of
    BadQuantity,       // quantity does not render as a venue-legal decimal
    BadPrice,
    BufferTooSmall, // payload exceeded the scratch buffer (a config bug)
    NotSigned,      // credentials were never loaded
    NothingToSend,  // a batch with no entries
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// OKX's instrument type. Exhaustive over MarketType on purpose: a new market
// must not compile silently into the swap path, and OPTION is a real listing
// here rather than a placeholder.
[[nodiscard]] std::string_view inst_type_of(MarketType market) noexcept;

// OKX's trade mode, which is NOT derivable from the instrument alone: the
// same SPOT order is tdMode "cash" in Spot and Futures account modes and
// "cross" in multi-currency and portfolio margin. Nothing in VenueConfig says
// which mode the account runs in, so this returns the Spot/Futures-mode
// answer and an account in portfolio margin will see its spot orders refused
// with 51000 until the mode becomes configurable.
//
// Options refuse rather than fall through: OKX prices an option as a premium
// in the underlying coin (BTC-USD options are ctType "inverse", ctVal 1 BTC),
// with px/pxUsd/pxVol as three mutually exclusive spellings of that premium,
// and the catalogue loader does not build OptionInstrument at all — so an
// option order has no tick size to divide by and no way to say which price it
// means. That is a missing model, not a missing parameter.
[[nodiscard]] std::expected<std::string_view, BuildError> td_mode_of(MarketType market) noexcept;

[[nodiscard]] std::string_view rest_host() noexcept;

// ISO-8601 UTC with millisecond precision — "2020-12-08T09:08:57.715Z", the
// exact 24 bytes OK-ACCESS-TIMESTAMP wants. Epoch milliseconds are accepted
// nowhere: OKX answers 50112 "Invalid OK-ACCESS-TIMESTAMP", and a local-time
// rendering answers 50102 an hour later, which is the same symptom a stale
// clock produces.
[[nodiscard]] std::string_view iso8601_ms(UnixNanos now, std::span<char, 24> out) noexcept;

class RequestBuilder {
public:
    RequestBuilder() = default;

    explicit RequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    // The passphrase travels in a header rather than into a digest, so it is
    // held as a view over the caller's Secret instead of copied into a second
    // buffer this class would then have to zeroize.
    void set_passphrase(std::string_view passphrase) noexcept { passphrase_ = passphrase; }

    // Demo trading is a header on the live host, not another host.
    void set_simulated(bool on) noexcept { simulated_ = on; }

    // Public reads ------------------------------------------------------

    [[nodiscard]] net::RestRequest instruments(std::string_view inst_type) noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest mark_price(std::string_view inst_id) noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view inst_id) noexcept;

    [[nodiscard]] net::RestRequest books(std::string_view inst_id, std::uint32_t depth) noexcept;

    // Order entry -------------------------------------------------------

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // OKX caps a cancel batch at 20 entries and its body is a bare JSON
    // ARRAY, not an object. Returns how many commands the frame consumed so
    // the caller loops rather than silently dropping the tail.
    struct Batch {
        net::RestRequest request;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Batch, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    // Reconciliation pulls ----------------------------------------------

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    fills(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market,
                                                                        UnixNanos now);

    // The account book. No `type` filter: one call returns funding and fees
    // together, and asking twice would read the two halves at two instants.
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_book(MarketType market, UnixNanos since, UnixNanos now);

    // Exposed for tests: the exact bytes that went into the last HMAC.
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view path, std::string_view body, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view path, std::string_view query, UnixNanos now);

    [[nodiscard]] std::vector<net::HttpHeader> headers(std::string_view timestamp,
                                                       std::string_view signature) const;

    const net::Signer* signer_{nullptr};
    std::string_view passphrase_;
    bool simulated_{false};

    // Scratch, reused per build: one request must be sent before the next is
    // built, which is what the cold-path await_response loop already does.
    std::array<char, 256> target_{};
    std::array<char, 4096> body_{};
    std::array<char, 4096 + 256> sign_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::okx
