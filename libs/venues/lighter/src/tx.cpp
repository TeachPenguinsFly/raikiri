#include <raikiri/venues/lighter/tx.hpp>

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace raikiri::venues::lighter {

namespace {

constexpr std::uint64_t k_attribute_skip_nonce = 4;

[[nodiscard]] std::uint64_t field(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value);
}

// the venue folds attributes into the hash only when one is set
[[nodiscard]] Digest with_attributes(const Digest& body, const TxHeader& header) noexcept {
    if (!header.skip_nonce) {
        return body;
    }
    const std::array<std::uint64_t, 8> attributes{k_attribute_skip_nonce, 1, 0, 0, 0, 0, 0, 0};
    const auto attributes_hash = hash_to_quintic(attributes);
    std::array<std::uint64_t, 10> both{};
    for (std::size_t i = 0; i < 40; ++i) {
        both[i / 8] |= static_cast<std::uint64_t>(body[i]) << (8 * (i % 8));
        both[5 + i / 8] |= static_cast<std::uint64_t>(attributes_hash[i]) << (8 * (i % 8));
    }
    return hash_to_quintic(both);
}

void append_field(std::string& out, std::string_view name, std::int64_t value) {
    out += '"';
    out += name;
    out += "\":";
    out += std::to_string(value);
    out += ',';
}

void append_header_tail(std::string& out, const TxHeader& header, const Signature& sig) {
    append_field(out, "ExpiredAt", header.expired_at);
    append_field(out, "Nonce", header.nonce);
    std::array<char, 4 * ((k_signature_bytes + 2) / 3) + 1> encoded{};
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), sig.data(), static_cast<int>(sig.size()));
    out += "\"Sig\":\"";
    out.append(encoded.data(), written > 0 ? static_cast<std::size_t>(written) : 0);
    out += "\",\"L2TxAttributes\":";
    out += header.skip_nonce ? "{\"4\":1}}" : "null}";
}

} // namespace

std::string to_hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 0x0FU];
    }
    return out;
}

Digest tx_hash(const CreateOrderTx& tx, std::uint32_t chain) noexcept {
    const std::array<std::uint64_t, 16> elements{chain,
                                                 k_tx_create_order,
                                                 field(tx.header.nonce),
                                                 field(tx.header.expired_at),
                                                 field(tx.header.account_index),
                                                 tx.header.api_key_index,
                                                 field(tx.market_index),
                                                 field(tx.client_order_index),
                                                 field(tx.base_amount),
                                                 tx.price,
                                                 tx.is_ask,
                                                 tx.type,
                                                 tx.time_in_force,
                                                 tx.reduce_only,
                                                 tx.trigger_price,
                                                 field(tx.order_expiry)};
    return with_attributes(hash_to_quintic(elements), tx.header);
}

Digest tx_hash(const CancelOrderTx& tx, std::uint32_t chain) noexcept {
    const std::array<std::uint64_t, 8> elements{chain,
                                                k_tx_cancel_order,
                                                field(tx.header.nonce),
                                                field(tx.header.expired_at),
                                                field(tx.header.account_index),
                                                tx.header.api_key_index,
                                                field(tx.market_index),
                                                field(tx.index)};
    return with_attributes(hash_to_quintic(elements), tx.header);
}

std::string tx_info(const CreateOrderTx& tx, const Signature& sig) {
    std::string out{"{"};
    out.reserve(512);
    append_field(out, "AccountIndex", tx.header.account_index);
    append_field(out, "ApiKeyIndex", tx.header.api_key_index);
    append_field(out, "MarketIndex", tx.market_index);
    append_field(out, "ClientOrderIndex", tx.client_order_index);
    append_field(out, "BaseAmount", tx.base_amount);
    append_field(out, "Price", tx.price);
    append_field(out, "IsAsk", tx.is_ask);
    append_field(out, "Type", tx.type);
    append_field(out, "TimeInForce", tx.time_in_force);
    append_field(out, "ReduceOnly", tx.reduce_only);
    append_field(out, "TriggerPrice", tx.trigger_price);
    append_field(out, "OrderExpiry", tx.order_expiry);
    append_header_tail(out, tx.header, sig);
    return out;
}

std::string tx_info(const CancelOrderTx& tx, const Signature& sig) {
    std::string out{"{"};
    out.reserve(320);
    append_field(out, "AccountIndex", tx.header.account_index);
    append_field(out, "ApiKeyIndex", tx.header.api_key_index);
    append_field(out, "MarketIndex", tx.market_index);
    append_field(out, "Index", tx.index);
    append_header_tail(out, tx.header, sig);
    return out;
}

std::string
auth_token(const ApiKey& key, std::int64_t deadline_s, std::int64_t account_index, std::uint8_t api_key_index) {
    auto message = std::to_string(deadline_s) + ':' + std::to_string(account_index) + ':' +
                   std::to_string(api_key_index);
    std::vector<std::uint64_t> elements((message.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < message.size(); ++i) {
        elements[i / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(message[i])) << (8 * (i % 8));
    }
    const auto sig = key.sign(hash_to_quintic(elements));
    message += ':';
    message += to_hex(sig);
    return message;
}

} // namespace raikiri::venues::lighter
