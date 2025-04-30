#pragma once

// Append-only mmap journal: durability, replay, and the hot->cold data feed in
// one mechanism.
//
// Hot append = memcpy into the mapping + release-store of committed_bytes.
// No syscall, no fsync on the hot path; a cold thread calls flush() on a
// 50 ms cadence (group commit). The durability window is explicit and
// documented: the venue is the ultimate system of record and reconciliation
// covers the gap — what the journal must never do is present a torn frame
// as valid, and the CRC walk at open guarantees that.

#include <cstddef>
#include <cstdint>
#include <span>

namespace raikiri::journal {

inline constexpr std::uint64_t k_file_magic = 0x5241494B4A4E4C31ULL; // "RAIKJNL1"
inline constexpr std::uint32_t k_file_abi = 1;
inline constexpr std::size_t k_header_bytes = 4096;
inline constexpr std::size_t k_frame_header_bytes = 24; // len4 schema2 ver2 seq8 ts8
inline constexpr std::size_t k_frame_trailer_bytes = 4; // crc32c
inline constexpr std::size_t k_max_payload = std::size_t{64} * 1024;
inline constexpr std::size_t k_default_segment_bytes = 256ULL << 20;

enum class JournalError : std::uint8_t {
    OpenFailed,
    MapFailed,
    BadMagic,
    BadAbi,
    PayloadTooLarge,
    RotateFailed,
    NoSegments,
};

// Total on-disk size of a frame carrying `payload_len` bytes.
[[nodiscard]] constexpr std::size_t frame_bytes(std::size_t payload_len) noexcept {
    const std::size_t raw = k_frame_header_bytes + payload_len + k_frame_trailer_bytes;
    return (raw + 7U) & ~std::size_t{7U};
}

// Decoded view of one frame; `payload` points into the reader's mapping and
// is valid until the reader advances past the segment.
struct FrameView {
    std::uint16_t schema_id{0};
    std::uint16_t schema_version{0};
    std::uint64_t seq{0};
    std::uint64_t ts_ns{0};
    std::span<const std::byte> payload{};
};

} // namespace raikiri::journal
