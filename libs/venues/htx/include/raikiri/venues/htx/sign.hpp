#pragma once

// rest and both private sockets sign this canonical string with hmac-sha256

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::htx::sign {

inline constexpr std::size_t k_max_params = 10;

// uppercase hex: htx signs %3A, and %3a gives a different digest
[[nodiscard]] std::string_view encode(std::string_view text, std::span<char> out) noexcept;

// colons left unencoded; Query::render encodes them
[[nodiscard]] std::string_view timestamp(std::uint64_t epoch_s, std::span<char> out) noexcept;

class Query {
public:
    void add(std::string_view name, std::string_view value) noexcept;

    // empty on overflow so a truncated query is never signed
    [[nodiscard]] std::string_view render(std::span<char> out) noexcept;

private:
    struct Param {
        std::string_view name;
        std::string_view value;
    };

    std::array<Param, k_max_params> params_{};
    std::size_t count_{0};
    bool overflow_{false};
};

// method, host, path and query joined by newlines; empty on overflow
[[nodiscard]] std::string_view canonical(std::string_view method,
                                         std::string_view host,
                                         std::string_view path,
                                         std::string_view query,
                                         std::span<char> out) noexcept;

} // namespace raikiri::venues::htx::sign
