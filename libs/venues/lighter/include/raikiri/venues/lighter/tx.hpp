#pragma once

#include <raikiri/venues/lighter/sign.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace raikiri::venues::lighter {

inline constexpr std::uint32_t k_chain_live = 304;
inline constexpr std::uint32_t k_chain_test = 300;

[[nodiscard]] constexpr std::uint32_t chain_id(bool testnet) noexcept {
    return testnet ? k_chain_test : k_chain_live;
}

inline constexpr std::uint8_t k_tx_create_order = 14;
inline constexpr std::uint8_t k_tx_cancel_order = 15;

inline constexpr std::uint8_t k_order_limit = 0;
inline constexpr std::uint8_t k_order_market = 1;

inline constexpr std::uint8_t k_tif_ioc = 0;
inline constexpr std::uint8_t k_tif_gtt = 1;
inline constexpr std::uint8_t k_tif_post_only = 2;

inline constexpr std::int64_t k_max_client_order_index = (std::int64_t{1} << 48) - 1;

struct TxHeader {
    std::int64_t account_index{0};
    std::uint8_t api_key_index{0};
    // ms; the venue drops the tx after it
    std::int64_t expired_at{0};
    std::int64_t nonce{0};
    // the tx then leaves the key's nonce where it was
    bool skip_nonce{false};
};

struct CreateOrderTx {
    TxHeader header{};
    std::int16_t market_index{0};
    std::int64_t client_order_index{0};
    std::int64_t base_amount{0};
    std::uint32_t price{0};
    std::uint8_t is_ask{0};
    std::uint8_t type{k_order_limit};
    std::uint8_t time_in_force{k_tif_ioc};
    std::uint8_t reduce_only{0};
    std::uint32_t trigger_price{0};
    // ms; zero for ioc, a deadline for a resting order
    std::int64_t order_expiry{0};
};

// `index` is a client order index below 2^48 or the venue's order index above it
struct CancelOrderTx {
    TxHeader header{};
    std::int16_t market_index{0};
    std::int64_t index{0};
};

[[nodiscard]] Digest tx_hash(const CreateOrderTx& tx, std::uint32_t chain) noexcept;
[[nodiscard]] Digest tx_hash(const CancelOrderTx& tx, std::uint32_t chain) noexcept;

// the tx_info json, field for field as the reference signer marshals it
[[nodiscard]] std::string tx_info(const CreateOrderTx& tx, const Signature& sig);
[[nodiscard]] std::string tx_info(const CancelOrderTx& tx, const Signature& sig);

// "deadline:account:key:sig", deadline in unix seconds; read endpoints and private channels take it
[[nodiscard]] std::string
auth_token(const ApiKey& key, std::int64_t deadline_s, std::int64_t account_index, std::uint8_t api_key_index);

[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);

} // namespace raikiri::venues::lighter
