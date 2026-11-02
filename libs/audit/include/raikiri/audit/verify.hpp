#pragma once

// a stream is a directory: <YYYY-MM-DD>.jsonl per day, HEAD with the last seq and hash written, LOCK held by the writer

#include <raikiri/audit/chain.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::audit {

namespace internal {

[[nodiscard]] std::optional<std::string> read_file(const std::string& path);
[[nodiscard]] std::optional<Tip> tail_of(const std::string& path);

} // namespace internal

// ascending; empty when the directory is missing
[[nodiscard]] std::vector<std::uint32_t> list_days(const std::string& stream_dir);

[[nodiscard]] std::string day_path(const std::string& stream_dir, std::uint32_t day);
[[nodiscard]] std::string head_path(const std::string& stream_dir);

[[nodiscard]] std::optional<Tip> read_head(const std::string& stream_dir);

// what an earlier walk of a day already covered, so the next one reads only what was added
struct VerifiedPrefix {
    std::uint64_t bytes{0};
    Tip tip{};
    ChainReport report{};
};

// a day's first line is checked against the last line of the day before it, and the newest day against HEAD. with
// `prefix`, lines it covers are taken as walked and it moves forward over what this walk found whole: an edit behind
// it shows only in a walk without one, which is what auditcheck does
[[nodiscard]] ChainReport
verify_day(const std::string& stream_dir, std::uint32_t day, VerifiedPrefix* prefix = nullptr);

// one report per day, ascending, ending at the first break
[[nodiscard]] std::vector<ChainReport> verify_all(const std::string& stream_dir);

} // namespace raikiri::audit
