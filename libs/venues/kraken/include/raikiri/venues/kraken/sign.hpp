#pragma once

// authent = base64(hmac-sha512(base64_decode(secret), sha256(message))), over raw digest bytes

#include <cstddef>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// base64 of a 64-byte digest: 88 characters including the single '=' pad.
inline constexpr std::size_t k_authent_bytes = 88;

// rest signs postData + nonce + path without /derivatives; the ws challenge signs the uuid alone
// empty on a bad secret or a short out
[[nodiscard]] std::string_view
authent(std::string_view secret_base64, std::string_view message, std::span<char> out) noexcept;

} // namespace raikiri::venues::kraken
