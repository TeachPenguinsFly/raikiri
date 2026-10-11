#include <raikiri/venues/hyperliquid/sign.hpp>

#include <cstring>
#include <memory>
#include <vector>

namespace raikiri::venues::hyperliquid {

namespace {

constexpr std::size_t k_address_bytes = 20;

void push_word(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> value) noexcept {
    const auto pad = k_hash_bytes - value.size();
    out.insert(out.end(), pad, 0);
    out.insert(out.end(), value.begin(), value.end());
}

void push_u64_word(std::vector<std::uint8_t>& out, std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8> be{};
    for (std::size_t i = 0; i < be.size(); ++i) {
        be[i] = static_cast<std::uint8_t>(value >> (8U * (7 - i)));
    }
    push_word(out, be);
}

[[nodiscard]] Hash hash_text(std::string_view text) noexcept {
    return keccak256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}); // NOLINT
}

// chainId 1337 and a zero verifying contract; computed with the same keccak the tests pin
const Hash& domain_separator() noexcept {
    static const Hash separator = [] {
        std::vector<std::uint8_t> encoded;
        encoded.reserve(5 * k_hash_bytes);
        const auto type_hash = hash_text(
            "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
        push_word(encoded, type_hash);
        push_word(encoded, hash_text("Exchange"));
        push_word(encoded, hash_text("1"));
        push_u64_word(encoded, 1337);
        push_word(encoded, std::span<const std::uint8_t>{}); // address(0)
        return keccak256(encoded);
    }();
    return separator;
}

} // namespace

bool same_address(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size() || a.empty()) {
        return false;
    }
    const auto lower = [](char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

Hash action_hash(std::span<const std::uint8_t> action,
                 std::uint64_t nonce,
                 std::string_view vault_address,
                 std::uint64_t expires_after) noexcept {
    std::vector<std::uint8_t> preimage;
    preimage.reserve(action.size() + 38);
    preimage.insert(preimage.end(), action.begin(), action.end());
    for (std::size_t i = 0; i < 8; ++i) {
        preimage.push_back(static_cast<std::uint8_t>(nonce >> (8U * (7 - i))));
    }
    std::array<std::uint8_t, k_address_bytes> vault{};
    if (!vault_address.empty() && net::evm::from_hex(vault_address, vault)) {
        preimage.push_back(0x01);
        preimage.insert(preimage.end(), vault.begin(), vault.end());
    } else {
        preimage.push_back(0x00);
    }
    if (expires_after != 0) {
        // the expiry marker is 0x00, unlike the vault marker
        preimage.push_back(0x00);
        for (std::size_t i = 0; i < 8; ++i) {
            preimage.push_back(static_cast<std::uint8_t>(expires_after >> (8U * (7 - i))));
        }
    }
    return keccak256(preimage);
}

Hash agent_digest(const Hash& action, bool mainnet) noexcept {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(3 * k_hash_bytes);
    push_word(encoded, hash_text("Agent(string source,bytes32 connectionId)"));
    // "a" mainnet, "b" testnet; keeps testnet orders off production
    push_word(encoded, hash_text(mainnet ? "a" : "b"));
    push_word(encoded, action);
    const auto struct_hash = keccak256(encoded);

    std::vector<std::uint8_t> message;
    message.reserve(2 + 2 * k_hash_bytes);
    message.push_back(0x19);
    message.push_back(0x01);
    const auto& domain = domain_separator();
    message.insert(message.end(), domain.begin(), domain.end());
    message.insert(message.end(), struct_hash.begin(), struct_hash.end());
    return keccak256(message);
}

} // namespace raikiri::venues::hyperliquid
