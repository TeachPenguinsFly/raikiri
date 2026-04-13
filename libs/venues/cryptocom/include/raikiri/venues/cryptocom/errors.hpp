#pragma once

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::cryptocom {

using WireError = wire::Error;

inline constexpr std::int64_t k_ok = 0;
inline constexpr std::int64_t k_bad_request = 40001;
inline constexpr std::int64_t k_method_not_found = 40002;
// Also an unsupported book depth/cadence pair, with the channel named.
inline constexpr std::int64_t k_invalid_request = 40003;
inline constexpr std::int64_t k_missing_argument = 40004;
inline constexpr std::int64_t k_unauthorized = 40101;
inline constexpr std::int64_t k_invalid_nonce = 40102;
inline constexpr std::int64_t k_ip_illegal = 40103;
inline constexpr std::int64_t k_max_subscriptions = 40107;
inline constexpr std::int64_t k_request_timeout = 40801;
inline constexpr std::int64_t k_too_many_requests = 42901;
inline constexpr std::int64_t k_internal = 50001;

// Unknown codes are Terminal.
[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

} // namespace raikiri::venues::cryptocom
