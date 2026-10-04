#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace raikiri::exec {

// the venue's time in unix nanoseconds, or nothing when the body does not carry it
using ParseServerTime = std::optional<std::uint64_t> (*)(std::string_view body);

struct ServerTimeQuery {
    std::string host{};
    std::string target{};
    // a non-empty body is POSTed as json
    std::string body{};
    std::uint64_t resolution_ns{1'000'000};
    ParseServerTime parse{nullptr};
};

// the adapter's server-time request and the offset its signer adds to the local clock
class VenueClock {
public:
    explicit VenueClock(ServerTimeQuery query, std::shared_ptr<std::int64_t> signing_offset = {})
        : query_{std::move(query)}, signing_offset_{std::move(signing_offset)} {}

    [[nodiscard]] const ServerTimeQuery& query() const noexcept { return query_; }

    // on the adapter's thread; a venue that signs on its own clock ignores it
    void use_offset(std::int64_t offset_ns) const noexcept {
        if (signing_offset_ != nullptr) {
            *signing_offset_ = offset_ns;
        }
    }

    [[nodiscard]] bool signs_with_offset() const noexcept { return signing_offset_ != nullptr; }

private:
    ServerTimeQuery query_;
    std::shared_ptr<std::int64_t> signing_offset_;
};

// an adapter's millisecond parser, as nanoseconds; zero counts as missing
template <typename Parsed> [[nodiscard]] std::optional<std::uint64_t> ms_to_ns(const Parsed& ms) noexcept {
    if (!ms.has_value() || *ms == 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*ms) * 1'000'000ULL;
}

// the first number after `"key":`, quoted or not, times `unit_ns`; an empty key reads a bare number
[[nodiscard]] std::optional<std::uint64_t>
json_number(std::string_view body, std::string_view key, std::uint64_t unit_ns = 1) noexcept;

// decimal seconds such as 1791126462.58, as nanoseconds
[[nodiscard]] std::optional<std::uint64_t> json_seconds(std::string_view body, std::string_view key) noexcept;

} // namespace raikiri::exec
