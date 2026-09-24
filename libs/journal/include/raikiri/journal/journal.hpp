#pragma once

// hot append is a memcpy plus a release store of committed_bytes; a cold thread flushes

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
    AlreadyWriting, // another live process holds this journal
};

[[nodiscard]] constexpr std::size_t frame_bytes(std::size_t payload_len) noexcept {
    const std::size_t raw = k_frame_header_bytes + payload_len + k_frame_trailer_bytes;
    return (raw + 7U) & ~std::size_t{7U};
}

// `payload` points into the reader's mapping until it leaves the segment
struct FrameView {
    std::uint16_t schema_id{0};
    std::uint16_t schema_version{0};
    std::uint64_t seq{0};
    std::uint64_t ts_ns{0};
    std::span<const std::byte> payload{};
};

} // namespace raikiri::journal
