#include <raikiri/clientapi/domain/key.hpp>

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::clientapi {

namespace {

std::optional<std::array<std::uint8_t, 16>> parse_address(const std::string& text, bool& was_v4) {
    std::array<std::uint8_t, 16> out{};
    if (::inet_pton(AF_INET6, text.c_str(), out.data()) == 1) {
        was_v4 = false;
        return out;
    }
    std::array<std::uint8_t, 4> v4{};
    if (::inet_pton(AF_INET, text.c_str(), v4.data()) != 1) {
        return std::nullopt;
    }
    was_v4 = true;
    out.fill(0);
    out[10] = 0xFF;
    out[11] = 0xFF;
    std::memcpy(out.data() + 12, v4.data(), 4);
    return out;
}

bool matches_pattern(std::string_view pattern, std::string_view symbol) noexcept {
    if (pattern.ends_with('*')) {
        return symbol.starts_with(pattern.substr(0, pattern.size() - 1));
    }
    return pattern == symbol;
}

} // namespace

std::optional<IpRange> parse_ip_range(std::string_view text) {
    const auto slash = text.find('/');
    bool was_v4 = false;
    const auto address = parse_address(std::string{text.substr(0, slash)}, was_v4);
    if (!address.has_value()) {
        return std::nullopt;
    }
    IpRange range{.address = *address, .bits = 128};
    if (slash != std::string_view::npos) {
        const auto tail = text.substr(slash + 1);
        unsigned bits = 0;
        const auto [stop, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), bits);
        if (ec != std::errc{} || stop != tail.data() + tail.size() || bits > (was_v4 ? 32U : 128U)) {
            return std::nullopt;
        }
        range.bits = static_cast<std::uint8_t>(was_v4 ? bits + 96U : bits);
    }
    return range;
}

bool in_range(const IpRange& range, std::string_view ip) {
    bool was_v4 = false;
    const auto address = parse_address(std::string{ip}, was_v4);
    if (!address.has_value()) {
        return false;
    }
    const std::size_t whole = range.bits / 8U;
    if (std::memcmp(address->data(), range.address.data(), whole) != 0) {
        return false;
    }
    const auto rest = static_cast<unsigned>(range.bits % 8U);
    if (rest == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - rest));
    return ((*address)[whole] & mask) == (range.address[whole] & mask);
}

bool in_any_range(const std::vector<IpRange>& ranges, std::string_view ip) {
    return std::ranges::any_of(ranges, [ip](const IpRange& range) { return in_range(range, ip); });
}

bool has(const ApiKey& key, Permission permission) noexcept {
    return (key.permissions & static_cast<std::uint8_t>(permission)) != 0;
}

bool allows_account(const ApiKey& key, std::string_view account) noexcept {
    return std::ranges::any_of(key.accounts,
                               [account](const std::string& allowed) { return allowed == "*" || allowed == account; });
}

bool allows_symbol(const ApiKey& key, std::string_view symbol) noexcept {
    return std::ranges::any_of(key.symbols, [symbol](const std::string& allowed) {
        return allowed == "*" || matches_pattern(allowed, symbol);
    });
}

bool all_accounts(const ApiKey& key) noexcept {
    return std::ranges::find(key.accounts, std::string_view{"*"}) != key.accounts.end();
}

bool all_symbols(const ApiKey& key) noexcept {
    return std::ranges::find(key.symbols, std::string_view{"*"}) != key.symbols.end();
}

bool allows_underlying(const ApiKey& key, std::string_view underlying) noexcept {
    if (underlying.empty()) {
        return false;
    }
    return std::ranges::any_of(key.symbols, [underlying](const std::string& allowed) {
        return allowed == "*" || (allowed.starts_with(underlying) && allowed.size() > underlying.size() &&
                                  (allowed[underlying.size()] == '/' || allowed[underlying.size()] == '-'));
    });
}

} // namespace raikiri::clientapi
