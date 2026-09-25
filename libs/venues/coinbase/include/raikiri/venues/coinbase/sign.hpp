#pragma once

// prehash = timestamp + METHOD + requestPath + body, signed with the base64-decoded secret;
// timestamp in seconds within 30 s of the venue; the ws signs GET /users/self/verify

#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// the path the websocket handshake signs
inline constexpr std::string_view k_ws_signature_path = "/users/self/verify";

// a secret decodes to 64 bytes; room for longer keys
inline constexpr std::size_t k_max_secret_bytes = 128;

// nullopt for non-base64 input, usually a mis-pasted key
[[nodiscard]] std::optional<std::size_t> decode_secret(std::string_view base64_secret, std::span<char> out) noexcept;

// empty on overflow
[[nodiscard]] std::string_view prehash(std::string_view timestamp,
                                       net::HttpMethod method,
                                       std::string_view request_path,
                                       std::string_view body,
                                       std::span<char> out) noexcept;

// whole seconds; the window is 30 s
[[nodiscard]] std::string_view timestamp_s(UnixNanos now, std::span<char> out) noexcept;

} // namespace raikiri::venues::coinbase
