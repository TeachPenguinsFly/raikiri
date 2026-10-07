#pragma once

// a browser sends no basic auth on a websocket upgrade, so a route behind it hands out a short-lived token

#include <raikiri/common/time.hpp>
#include <raikiri/config/secret.hpp>
#include <raikiri/http/server.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::admin {

enum class TokenCheck : std::uint8_t { Valid, Missing, Malformed, Expired, BadSignature };

[[nodiscard]] std::string_view to_string(TokenCheck check) noexcept;

// it only has to outlive the fetch that precedes the upgrade
inline constexpr std::uint64_t k_stream_token_ttl_ns = 60ULL * 1'000'000'000ULL;

struct IssuedToken {
    std::string token{};
    UnixNanos expires_at{};
};

// "<expiry ns>.<nonce>.<hmac-sha256 of the first two, hex>"
class StreamTokens {
public:
    StreamTokens() = default;
    StreamTokens(const StreamTokens&) = delete;
    StreamTokens& operator=(const StreamTokens&) = delete;

    [[nodiscard]] config::Secret& secret() noexcept { return secret_; }

    // without a configured secret; outstanding tokens then die with the process
    [[nodiscard]] bool randomize() noexcept;

    [[nodiscard]] IssuedToken issue(UnixNanos now) const;
    [[nodiscard]] TokenCheck check(std::string_view token, UnixNanos now) const noexcept;

private:
    config::Secret secret_;
};

inline constexpr std::string_view k_stream_token_path = "/v1/stream/token";

// empty for any other path so a composed router can fall through
class StreamTokenRoute {
public:
    StreamTokenRoute(const StreamTokens& tokens, std::function<UnixNanos()> wall)
        : tokens_{&tokens}, wall_{std::move(wall)} {}

    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    const StreamTokens* tokens_;
    std::function<UnixNanos()> wall_;
};

} // namespace raikiri::admin
