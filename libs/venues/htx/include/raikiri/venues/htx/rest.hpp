#pragma once

// signatures travel as query parameters over the name-sorted, percent-encoded query
// (timestamp colons as %3A); a post body is not signed

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

// derivatives host; catalogue and market data use it too
[[nodiscard]] std::string_view rest_host() noexcept;

[[nodiscard]] std::string_view spot_rest_host() noexcept;

[[nodiscard]] std::string_view rest_host(MarketType market) noexcept;

// cross margin; isolated needs a config field plus the non-_cross endpoints and topics
inline constexpr std::string_view k_margin_mode = "cross";

inline constexpr std::string_view k_cross_margin_account = "USDT";

inline constexpr std::string_view k_funding_record_types = "30,31";

// htx refuses an order whose lever_rate differs from an open position's leverage
inline constexpr std::int32_t k_lever_rate = 1;

// valid only in one-way position mode; hedge-mode orders are refused
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
    // a spot market buy is priced in quote currency and an OrderCmd carries base lots
    SpotMarketBuyPricedInQuote,
    MixedBatch,         // one cancel batch straddling instruments
    BatchTooLarge,      // more ids than the endpoint accepts in one call
    SpotHasNoPositions, // a spot balance is not a position HTX can report
    SpotHasNoFunding,   // spot pays no funding
    AmendUnsupported,
    OptionsUnsupported,      // HTX lists options; this adapter does not trade them
    DatedFuturesUnsupported, // the dated-futures surface is a separate API
};

[[nodiscard]] std::string_view to_string(BuildError err) noexcept;

// checked first so an unserved market is not reported as UnknownInstrument
[[nodiscard]] std::optional<BuildError> market_refusal(MarketType market) noexcept;

// swap client_order_id must be a positive int64, so the strategy's u64 ClientOrderId goes instead of the ulid
[[nodiscard]] std::optional<std::int64_t> swap_client_id(ClientOrderId id) noexcept;

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

    // zero until learned at start(); spot order entry refuses until then
    void set_spot_account_id(std::int64_t id) noexcept { spot_account_id_ = id; }

    [[nodiscard]] std::int64_t spot_account_id() const noexcept { return spot_account_id_; }

    [[nodiscard]] std::expected<net::RestRequest, BuildError> spot_accounts(UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    create_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // always refuses; neither htx api can amend a live order
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    amend_order(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now);

    // one instrument per batch: the swap endpoint takes contract_code beside the ids
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    cancel_batch(std::span<const OrderCmd> cmds, const exec::InstrumentTable& table, UnixNanos now);

    // swap pages at 50 rows; page is 1-based
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    open_orders(MarketType market, std::uint32_t page, UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> query_order(const ExchangeClientOrderId& id,
                                                                          std::int64_t numeric_id,
                                                                          std::string_view contract_code,
                                                                          MarketType market,
                                                                          UnixNanos now);

    [[nodiscard]] std::expected<net::RestRequest, BuildError> positions(MarketType market, UnixNanos now);

    // htx refuses windows wider than 48 hours, so since is clamped
    [[nodiscard]] std::expected<net::RestRequest, BuildError>
    financial_record(MarketType market, UnixNanos since, UnixNanos now);

    // for tests: the bytes that went into the last hmac
    [[nodiscard]] std::string_view last_sign_message() const noexcept;

    // swap cancel takes at most 10 ids; spot batchcancel takes 50
    inline static constexpr std::size_t k_max_swap_batch_cancel = 10;
    inline static constexpr std::size_t k_max_spot_batch_cancel = 50;

    // swap_cross_openorders page_size caps at 50
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
