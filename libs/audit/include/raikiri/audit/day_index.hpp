#pragma once

// a sidecar per day, <YYYY-MM-DD>.idx, so a query reads only the blocks that can match. it speeds reads and proves
// nothing: lines it does not cover are read anyway, and only the chain says what is true

#include <raikiri/audit/log.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::audit {

inline constexpr std::size_t k_block_lines = 512;
inline constexpr std::size_t k_block_bytes = std::size_t{256} * 1024;
inline constexpr std::size_t k_bloom_words = 128;

enum class IndexField : std::uint8_t { Actor, Action, Subject };

struct IndexBlock {
    std::uint64_t offset{0};
    std::uint64_t bytes{0};
    std::uint64_t min_ts{0};
    std::uint64_t max_ts{0};
    std::uint64_t lines{0};
    std::array<std::uint64_t, k_bloom_words> bloom{};

    void add(const Entry& entry, std::uint64_t at, std::uint64_t line_bytes) noexcept;

    [[nodiscard]] bool may_hold(IndexField field, std::string_view value) const noexcept;

    [[nodiscard]] bool full() const noexcept { return lines >= k_block_lines || bytes >= k_block_bytes; }
};

static_assert(sizeof(IndexBlock) == 40 + (k_bloom_words * 8));

[[nodiscard]] std::string index_path(const std::string& stream_dir, std::uint32_t day);

// ascending, within `file_bytes`; a missing or damaged sidecar reads as fewer blocks
[[nodiscard]] std::vector<IndexBlock> read_index(const std::string& path, std::uint64_t file_bytes);

// a day older than `keep_days` back from `today`, ascending
[[nodiscard]] std::vector<std::uint32_t>
days_past(const std::string& stream_dir, std::uint32_t keep_days, std::uint32_t today);

void remove_day(const std::string& stream_dir, std::uint32_t day);

} // namespace raikiri::audit
