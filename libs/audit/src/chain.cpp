#include <raikiri/audit/chain.hpp>

#include <openssl/sha.h>

#include <chrono>
#include <cstdio>

namespace raikiri::audit {

namespace internal {

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

} // namespace internal

Digest sha256(std::string_view bytes) noexcept {
    Digest out{};
    ::SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), out.data());
    return out;
}

std::string to_hex(const Digest& digest) {
    constexpr std::string_view k_digits = "0123456789abcdef";
    std::string out(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = k_digits[digest[i] >> 4U];
        out[(i * 2) + 1] = k_digits[digest[i] & 0x0FU];
    }
    return out;
}

std::optional<Digest> digest_from_hex(std::string_view text) noexcept {
    Digest out{};
    if (text.size() != out.size() * 2) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = internal::hex_value(text[i * 2]);
        const int low = internal::hex_value(text[(i * 2) + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return out;
}

std::string day_name(std::uint32_t day) {
    const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{day}}};
    std::array<char, 16> buf{};
    const int written = std::snprintf(buf.data(),
                                      buf.size(),
                                      "%04d-%02u-%02u",
                                      static_cast<int>(date.year()),
                                      static_cast<unsigned>(date.month()),
                                      static_cast<unsigned>(date.day()));
    return {buf.data(), static_cast<std::size_t>(written)};
}

std::optional<std::uint32_t> parse_day(std::string_view name) noexcept {
    if (name.size() != 10 || name[4] != '-' || name[7] != '-') {
        return std::nullopt;
    }
    int fields[3] = {0, 0, 0};
    const std::array<std::string_view, 3> parts{name.substr(0, 4), name.substr(5, 2), name.substr(8, 2)};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        for (const char c : parts[i]) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            fields[i] = (fields[i] * 10) + (c - '0');
        }
    }
    const std::chrono::year_month_day date{std::chrono::year{fields[0]},
                                           std::chrono::month{static_cast<unsigned>(fields[1])},
                                           std::chrono::day{static_cast<unsigned>(fields[2])}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch().count();
    if (days < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(days);
}

std::string_view to_string(BreakKind kind) noexcept {
    switch (kind) {
    case BreakKind::Malformed:
        return "malformed";
    case BreakKind::Sequence:
        return "sequence";
    case BreakKind::PrevHash:
        return "prev_hash";
    case BreakKind::Anchor:
        return "anchor";
    case BreakKind::Head:
        return "head";
    }
    return "malformed";
}

bool ChainWalk::line(std::uint64_t seq, const Digest& prev, const Digest& hash) noexcept {
    if (break_.has_value()) {
        return false;
    }
    const std::uint64_t at = lines_ + 1;
    if (lines_ == 0) {
        first_seq_ = seq;
        if (anchor_.has_value()) {
            if (seq != anchor_->seq + 1 || prev != anchor_->hash) {
                break_ = Break{
                    .kind = BreakKind::Anchor, .line = at, .expected_seq = anchor_->seq + 1, .found_seq = seq};
                return false;
            }
            anchored_ = true;
        } else if (seq == 0) {
            if (prev != k_genesis) {
                break_ = Break{.kind = BreakKind::Anchor, .line = at, .expected_seq = 0, .found_seq = 0};
                return false;
            }
            anchored_ = true;
        }
    } else {
        const std::uint64_t expected = tip_.seq + 1;
        if (seq != expected) {
            break_ = Break{.kind = BreakKind::Sequence, .line = at, .expected_seq = expected, .found_seq = seq};
            return false;
        }
        if (prev != tip_.hash) {
            break_ = Break{.kind = BreakKind::PrevHash, .line = at, .expected_seq = expected, .found_seq = seq};
            return false;
        }
    }
    tip_ = {.seq = seq, .hash = hash};
    lines_ = at;
    return true;
}

void ChainWalk::malformed() noexcept {
    if (!break_.has_value()) {
        const std::uint64_t expected = lines_ == 0 ? 0 : tip_.seq + 1;
        break_ = Break{.kind = BreakKind::Malformed, .line = lines_ + 1, .expected_seq = expected, .found_seq = 0};
    }
}

} // namespace raikiri::audit
