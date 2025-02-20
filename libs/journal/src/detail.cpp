#include <raikiri/journal/detail.hpp>

#include <crc32c/crc32c.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace raikiri::journal::detail {

bool mkdir_p(const std::string& dir) noexcept {
    std::string built;
    built.reserve(dir.size());
    for (std::size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!built.empty() && ::mkdir(built.c_str(), 0700) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (i < dir.size()) {
            built.push_back(dir[i]);
        }
    }
    return true;
}

std::string segment_path(const std::string& dir, const std::string& prefix,
                         std::uint64_t segment_seq) {
    char num[8];
    std::snprintf(num, sizeof num, "%06llu", static_cast<unsigned long long>(segment_seq));
    return dir + "/" + prefix + "." + num + ".rj";
}

std::vector<std::uint64_t> list_segments(const std::string& dir, const std::string& prefix) {
    std::vector<std::uint64_t> out;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        return out;
    }
    const std::string want_prefix = prefix + ".";
    while (const dirent* entry = ::readdir(d)) {
        const std::string_view name{entry->d_name};
        if (!name.starts_with(want_prefix) || !name.ends_with(".rj")) {
            continue;
        }
        const auto digits = name.substr(want_prefix.size(),
                                        name.size() - want_prefix.size() - 3);
        std::uint64_t seq{};
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), seq);
        if (ec == std::errc{} && ptr == digits.data() + digits.size()) {
            out.push_back(seq);
        }
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

bool decode_frame(const std::byte* base, std::size_t size, std::uint64_t off,
                  FrameView& out) noexcept {
    if (off + k_frame_header_bytes + k_frame_trailer_bytes > size) {
        return false;
    }
    const std::byte* frame = base + off;
    std::uint32_t len{};
    std::memcpy(&len, frame, 4);
    if (len > k_max_payload || off + frame_bytes(len) > size) {
        return false;
    }
    std::uint32_t stored_crc{};
    std::memcpy(&stored_crc, frame + k_frame_header_bytes + len, 4);
    const std::uint32_t crc =
        crc32c::Crc32c(reinterpret_cast<const char*>(frame), k_frame_header_bytes + len);
    if (crc != stored_crc) {
        return false;
    }
    std::memcpy(&out.schema_id, frame + 4, 2);
    std::memcpy(&out.schema_version, frame + 6, 2);
    std::memcpy(&out.seq, frame + 8, 8);
    std::memcpy(&out.ts_ns, frame + 16, 8);
    out.payload = {frame + k_frame_header_bytes, len};
    return true;
}

WalkResult walk_frames(const std::byte* base, std::size_t size,
                       std::uint64_t base_record_seq) noexcept {
    WalkResult result{};
    result.end_offset = k_header_bytes;
    result.next_seq = base_record_seq;

    std::uint64_t expect_seq = base_record_seq;
    std::uint64_t off = k_header_bytes;
    FrameView frame{};
    while (decode_frame(base, size, off, frame)) {
        if (frame.seq != expect_seq) {
            break; // stale bytes from a recycled region — treat as tail
        }
        off += frame_bytes(frame.payload.size());
        ++expect_seq;
    }
    result.end_offset = off;
    result.next_seq = expect_seq;
    return result;
}

} // namespace raikiri::journal::detail
