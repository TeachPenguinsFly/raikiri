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

} // namespace internal

// ascending; empty when the directory is missing
[[nodiscard]] std::vector<std::uint32_t> list_days(const std::string& stream_dir);

[[nodiscard]] std::string day_path(const std::string& stream_dir, std::uint32_t day);
[[nodiscard]] std::string head_path(const std::string& stream_dir);

[[nodiscard]] std::optional<Tip> read_head(const std::string& stream_dir);

// a day's first line is checked against the last line of the day before it, and the newest day against HEAD
[[nodiscard]] ChainReport verify_day(const std::string& stream_dir, std::uint32_t day);

// one report per day, ascending, ending at the first break
[[nodiscard]] std::vector<ChainReport> verify_all(const std::string& stream_dir);

} // namespace raikiri::audit
