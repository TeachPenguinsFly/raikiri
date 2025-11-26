#pragma once

// Shared internals of the journal writer/reader: segment file naming,
// directory listing, and the CRC frame walk that recovery and reading share.

#include <raikiri/journal/journal.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace raikiri::journal::detail {

[[nodiscard]] bool mkdir_p(const std::string& dir) noexcept;

[[nodiscard]] std::string
segment_path(const std::string& dir, const std::string& prefix, std::uint64_t segment_seq);

// A segment file that was created and never written to.
//
// ROTATION IS TWO STEPS — create the file, then write its header — and a
// process killed between them leaves exactly this: the right size, all zeros,
// no magic. It holds no frames, because a frame is only reachable through the
// header, so it is safe to initialise in place. Distinguishing it from real
// corruption matters: corruption is a file with a wrong magic, and that must
// still be refused rather than overwritten.
[[nodiscard]] bool segment_is_blank(const std::string& path) noexcept;

// Ascending segment seqs found under dir for prefix; empty when none.
[[nodiscard]] std::vector<std::uint64_t> list_segments(const std::string& dir,
                                                       const std::string& prefix);

struct WalkResult {
    std::uint64_t end_offset{k_header_bytes}; // first byte past the last valid frame
    std::uint64_t next_seq{1};                // seq the next appended record takes
};

// Walks frames from the header page, validating structure, CRC, and seq
// contiguity starting at base_record_seq. Stops (without failing) at the
// first invalid frame — that is the torn tail.
[[nodiscard]] WalkResult
walk_frames(const std::byte* base, std::size_t size, std::uint64_t base_record_seq) noexcept;

// Validates and decodes the frame at `off`. Returns false if the frame is
// structurally invalid or fails CRC (caller decides whether that means
// "caught up" or "corrupt" based on the committed watermark).
[[nodiscard]] bool
decode_frame(const std::byte* base, std::size_t size, std::uint64_t off, FrameView& out) noexcept;

} // namespace raikiri::journal::detail
