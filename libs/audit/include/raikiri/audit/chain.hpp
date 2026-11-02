#pragma once

// one writer owns a chain; each line names the sha-256 of the line before it
// the hash is unkeyed, so someone who can rewrite a whole chain can recompute it: only a tip copied elsewhere proves
// otherwise

#include <raikiri/common/time.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::audit {

using Digest = std::array<std::uint8_t, 32>;

inline constexpr Digest k_genesis{};

inline constexpr std::string_view k_action_recovery = "audit.recovery";
inline constexpr std::string_view k_action_overflow = "audit.overflow";
inline constexpr std::string_view k_action_retention = "audit.retention";

[[nodiscard]] Digest sha256(std::string_view bytes) noexcept;
[[nodiscard]] std::string to_hex(const Digest& digest);
[[nodiscard]] std::optional<Digest> digest_from_hex(std::string_view text) noexcept;

// days since the epoch, utc
[[nodiscard]] constexpr std::uint32_t day_of(UnixNanos ts) noexcept {
    return static_cast<std::uint32_t>(ts.ns / (86'400ULL * 1'000'000'000ULL));
}

[[nodiscard]] std::string day_name(std::uint32_t day);
[[nodiscard]] std::optional<std::uint32_t> parse_day(std::string_view name) noexcept;

struct Tip {
    std::uint64_t seq{0};
    Digest hash{};
};

enum class BreakKind : std::uint8_t {
    Malformed, // the line cannot be read as an entry
    Sequence,  // a line is missing or out of place
    PrevHash,  // the line before this one was edited
    Anchor,    // the day does not follow the last line of the day before
    Head,      // the last lines are gone, or the last line was edited
};

[[nodiscard]] std::string_view to_string(BreakKind kind) noexcept;

struct Break {
    BreakKind kind{BreakKind::Malformed};
    std::uint64_t line{0}; // 1-based, in the day's file
    std::uint64_t expected_seq{0};
    std::uint64_t found_seq{0};
};

class ChainWalk {
public:
    // `anchor` is the previous day's last line; without one, a first line at seq zero must carry the genesis hash
    // and any later first line is taken on trust and reported as unanchored
    explicit ChainWalk(std::optional<Tip> anchor) noexcept : anchor_{anchor} {}

    // false once the chain has broken
    bool line(std::uint64_t seq, const Digest& prev, const Digest& hash) noexcept;
    void malformed() noexcept;

    [[nodiscard]] const std::optional<Break>& first_break() const noexcept { return break_; }

    [[nodiscard]] std::uint64_t lines() const noexcept { return lines_; }

    [[nodiscard]] std::uint64_t first_seq() const noexcept { return first_seq_; }

    [[nodiscard]] const Tip& tip() const noexcept { return tip_; }

    [[nodiscard]] bool anchored() const noexcept { return anchored_; }

private:
    std::optional<Tip> anchor_;
    std::optional<Break> break_{};
    std::uint64_t lines_{0};
    std::uint64_t first_seq_{0};
    Tip tip_{};
    bool anchored_{false};
};

struct ChainReport {
    std::uint32_t day{0};
    bool found{false};
    std::uint64_t lines{0};
    std::uint64_t first_seq{0};
    std::uint64_t last_seq{0};
    Digest head{};
    bool anchored{false};
    bool head_checked{false};
    // a partial last line; a live writer can show one for an instant, and the next open truncates it
    std::uint64_t torn_tail_bytes{0};
    std::uint64_t recoveries{0};
    std::uint64_t overflows{0};
    std::optional<Break> first_break{};

    [[nodiscard]] bool ok() const noexcept { return !first_break.has_value(); }
};

} // namespace raikiri::audit
