#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::clientapi {

enum class Permission : std::uint8_t { Read = 1, Trade = 2, Algo = 4 };

struct RateSpec {
    std::uint32_t requests_per_second{20};
    std::uint32_t burst{40};
    std::uint32_t mutations_per_second{5};
    std::uint32_t mutation_burst{10};
};

// v4 is held as its v6-mapped form so one compare covers both
struct IpRange {
    std::array<std::uint8_t, 16> address{};
    std::uint8_t bits{128};
};

[[nodiscard]] std::optional<IpRange> parse_ip_range(std::string_view text);
[[nodiscard]] bool in_range(const IpRange& range, std::string_view ip);
[[nodiscard]] bool in_any_range(const std::vector<IpRange>& ranges, std::string_view ip);

// the secret is not here: only a KeyDirectory can check a signature
struct ApiKey {
    std::string id{};
    std::string client_id{};
    std::uint8_t permissions{0};
    std::vector<std::string> accounts{}; // names, or "*"
    std::vector<std::string> symbols{};  // local symbols, a trailing * as a prefix, or "*"
    RateSpec rate{};
    std::vector<IpRange> ips{}; // empty lets any address in
    std::uint32_t max_streams{4};
};

[[nodiscard]] bool has(const ApiKey& key, Permission permission) noexcept;
[[nodiscard]] bool allows_account(const ApiKey& key, std::string_view account) noexcept;
[[nodiscard]] bool allows_symbol(const ApiKey& key, std::string_view symbol) noexcept;
[[nodiscard]] bool all_accounts(const ApiKey& key) noexcept;
[[nodiscard]] bool all_symbols(const ApiKey& key) noexcept;

// an option's underlying is allowed when a pattern names its base asset
[[nodiscard]] bool allows_underlying(const ApiKey& key, std::string_view underlying) noexcept;

} // namespace raikiri::clientapi
