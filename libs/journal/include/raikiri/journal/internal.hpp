#pragma once

#include <raikiri/journal/journal.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace raikiri::journal::internal {

[[nodiscard]] bool mkdir_p(const std::string& dir) noexcept;

[[nodiscard]] std::string
segment_path(const std::string& dir, const std::string& prefix, std::uint64_t segment_seq);

// a file created by rotation whose header was never written; it holds no frames
[[nodiscard]] bool segment_is_blank(const std::string& path) noexcept;

// Ascending segment seqs found under dir for prefix; empty when none.
[[nodiscard]] std::vector<std::uint64_t> list_segments(const std::string& dir,
                                                       const std::string& prefix);

struct WalkResult {
    std::uint64_t end_offset{k_header_bytes}; // first byte past the last valid frame
    std::uint64_t next_seq{1};                // seq the next appended record takes
};

// stops without failing at the first invalid frame, the torn tail
[[nodiscard]] WalkResult
walk_frames(const std::byte* base, std::size_t size, std::uint64_t base_record_seq) noexcept;

// false on a structure or crc failure; the caller tells caught-up from corrupt by the committed
// watermark
[[nodiscard]] bool
decode_frame(const std::byte* base, std::size_t size, std::uint64_t off, FrameView& out) noexcept;

} // namespace raikiri::journal::internal
