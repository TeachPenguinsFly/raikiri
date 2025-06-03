#pragma once

// The canonical string every authenticated HTX surface shares.
//
// REST and BOTH private sockets sign
//
//     METHOD \n host \n path \n sorted-percent-encoded-query
//
// with HMAC-SHA256 and carry the digest base64-encoded. The spot socket only
// renames the parameters, so the machinery lives here rather than in whichever
// file happened to need it first.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::htx::sign {

// Six is the widest request this adapter builds; the four auth parameters are
// always among them.
inline constexpr std::size_t k_max_params = 10;

// Percent-encodes everything outside RFC 3986's unreserved set, with UPPERCASE
// hex. HTX's own worked example spells the timestamp's colon %3A, and %3a is a
// different byte string to the digest.
[[nodiscard]] std::string_view encode(std::string_view text, std::span<char> out) noexcept;

// Epoch seconds -> "2026-08-26T08:00:00", UTC and second precision. The colons
// are NOT encoded here: Query::render encodes every value on its way out, and
// encoding twice would sign %253A.
[[nodiscard]] std::string_view timestamp(std::uint64_t epoch_s, std::span<char> out) noexcept;

// Accumulates query parameters and renders them the way the signature demands.
class Query {
public:
    void add(std::string_view name, std::string_view value) noexcept;

    // Sorts by parameter NAME in ASCII order, percent-encodes every value and
    // writes "name=value&..." into `out`. Returns empty when anything did not
    // fit: a truncated query signs something we did not mean to send.
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

// METHOD \n host \n path \n query. Empty on overflow.
[[nodiscard]] std::string_view canonical(std::string_view method,
                                         std::string_view host,
                                         std::string_view path,
                                         std::string_view query,
                                         std::span<char> out) noexcept;

} // namespace raikiri::venues::htx::sign
