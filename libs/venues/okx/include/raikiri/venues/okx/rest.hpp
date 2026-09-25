#pragma once

// demo is an x-simulated-trading header, which public reads ignore.
// the signed requestPath includes the query with its '?'

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

// exhaustive so a new market cannot fall into the swap path
[[nodiscard]] std::string_view inst_type_of(MarketType market) noexcept;

// assumes spot/futures account mode, so spot orders under portfolio margin get 51000.
// options are refused: the catalogue builds no OptionInstrument
[[nodiscard]] std::expected<std::string_view, BuildError> td_mode_of(MarketType market) noexcept;

[[nodiscard]] std::string_view rest_host() noexcept;

// iso-8601 utc with ms, 24 bytes; epoch ms is refused with 50112
[[nodiscard]] std::string_view iso8601_ms(UnixNanos now, std::span<char, 24> out) noexcept;

class RequestBuilder {
public:
    RequestBuilder() = default;

    explicit RequestBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    RequestBuilder(const RequestBuilder&) = delete;
    RequestBuilder& operator=(const RequestBuilder&) = delete;

    // a view over the caller's Secret, so nothing here needs zeroizing
    void set_passphrase(std::string_view passphrase) noexcept { passphrase_ = passphrase; }

    // demo trading is a header on the live host
    void set_simulated(bool on) noexcept { simulated_ = on; }

    [[nodiscard]] net::RestRequest instruments(std::string_view inst_type) noexcept;

    [[nodiscard]] net::RestRequest server_time() noexcept;

    [[nodiscard]] net::RestRequest mark_price(std::string_view inst_id) noexcept;

    [[nodiscard]] net::RestRequest funding_rate(std::string_view inst_id) noexcept;

    [[nodiscard]] net::RestRequest books(std::string_view inst_id, std::uint32_t depth) noexcept;

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // okx caps a cancel batch at 20 and the body is a bare array; returns how many were consumed
    struct Batch {
        net::RestRequest request;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Batch, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> open_orders(MarketType market, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> fills(MarketType market, UnixNanos since, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    // no type filter: funding and fees come from one read
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    funding_book(MarketType market, UnixNanos since, UnixNanos now);

    // no ccy filter: spot and swap share the trading account
    [[nodiscard]] std::expected<net::RestRequest, BuildError> balances(UnixNanos now);

    // read beside the balance so a mid-session mode switch is seen
    [[nodiscard]] std::expected<net::RestRequest, BuildError> account_config(UnixNanos now);

    [[nodiscard]] std::string_view last_sign_message() const noexcept;

private:
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_post(std::string_view path, std::string_view body, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    signed_get(std::string_view path, std::string_view query, UnixNanos now);

    [[nodiscard]] std::vector<net::HttpHeader> headers(std::string_view timestamp, std::string_view signature) const;

    const net::Signer* signer_{nullptr};
    std::string_view passphrase_;
    bool simulated_{false};

    // scratch reused per build: send a request before building the next
    std::array<char, 256> target_{};
    std::array<char, 4096> body_{};
    std::array<char, 4096 + 256> sign_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t sign_msg_len_{0};
};

} // namespace raikiri::venues::okx
