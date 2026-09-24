#pragma once

// signature is lowercase hex hmac-sha256 of timestamp\nnonce\ndata; rest data is method\nuri\nbody\n.
// the timestamp is ms and must be under 60 s old, so it uses the venue-corrected clock

#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit::sign {

inline constexpr std::size_t k_max_auth_header = 288;

// empty on overflow so a truncated request is never signed
[[nodiscard]] std::string_view request_data(net::HttpMethod method,
                                            std::string_view uri,
                                            std::string_view body,
                                            std::span<char> out) noexcept;

[[nodiscard]] std::string_view message(std::uint64_t timestamp_ms,
                                       std::string_view nonce,
                                       std::string_view data,
                                       std::span<char> out) noexcept;

// deribit only requires the nonce not to repeat
[[nodiscard]] std::string_view nonce(std::uint64_t counter, std::span<char> out) noexcept;

[[nodiscard]] std::string_view auth_header(const net::Signer& signer,
                                           std::uint64_t timestamp_ms,
                                           std::string_view nonce,
                                           std::string_view signature,
                                           std::span<char> out) noexcept;

} // namespace raikiri::venues::deribit::sign
