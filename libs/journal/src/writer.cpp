#include <raikiri/common/platform.hpp>
#include <raikiri/journal/detail.hpp>
#include <raikiri/journal/writer.hpp>

#include <crc32c/crc32c.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

namespace raikiri::journal {

JournalWriter::JournalWriter(JournalWriter&& other) noexcept
    : dir_{std::move(other.dir_)}, prefix_{std::move(other.prefix_)},
      segment_bytes_{other.segment_bytes_}, header_{std::exchange(other.header_, nullptr)},
      base_{std::exchange(other.base_, nullptr)}, fd_{std::exchange(other.fd_, -1)},
      lock_fd_{std::exchange(other.lock_fd_, -1)}, write_off_{other.write_off_},
      segment_seq_{other.segment_seq_}, next_seq_{other.next_seq_} {}

JournalWriter::~JournalWriter() {
    close_segment();
    if (lock_fd_ >= 0) {
        ::close(lock_fd_); // closing releases the flock
        lock_fd_ = -1;
    }
}

// TWO WRITERS IS THE ONE UNRECOVERABLE CONFIGURATION. Creating a segment uses
// O_EXCL, which refuses a second creator — but that only guards the first run.
// On every run after it the files exist, so both processes take the recovery
// path, both mmap MAP_SHARED, and both append from their own cached offset:
// each overwrites the other's frames with no diagnostic anywhere. An advisory
// lock on a file beside the segments closes that, and it is on the journal
// rather than the current segment because the writer rolls segments and a lock
// that moves would leave a window mid-roll.
std::expected<void, JournalError> JournalWriter::claim(const std::string& dir,
                                                       const std::string& prefix) noexcept {
    const std::string path = dir + "/" + prefix + ".lock";
    lock_fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (lock_fd_ < 0) {
        return std::unexpected{JournalError::OpenFailed};
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock_fd_);
        lock_fd_ = -1;
        return std::unexpected{JournalError::AlreadyWriting};
    }
    return {};
}

void JournalWriter::close_segment() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, segment_bytes_);
        base_ = nullptr;
        header_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::expected<JournalWriter, JournalError>
JournalWriter::open(std::string dir, std::string prefix, std::size_t segment_bytes) {
    JournalWriter w;
    w.dir_ = std::move(dir);
    w.prefix_ = std::move(prefix);
    w.segment_bytes_ = segment_bytes;

    if (!detail::mkdir_p(w.dir_)) {
        return std::unexpected{JournalError::OpenFailed};
    }
    if (auto claimed = w.claim(w.dir_, w.prefix_); !claimed.has_value()) {
        return std::unexpected{claimed.error()};
    }

    const auto segments = detail::list_segments(w.dir_, w.prefix_);
    if (segments.empty()) {
        if (auto r = w.open_segment(0, /*recover=*/false); !r.has_value()) {
            return std::unexpected{r.error()};
        }
    } else {
        // Recover the newest segment; earlier ones are sealed history.
        if (auto r = w.open_segment(segments.back(), /*recover=*/true); !r.has_value()) {
            return std::unexpected{r.error()};
        }
    }
    return w;
}

std::expected<void, JournalError> JournalWriter::open_segment(std::uint64_t segment_seq,
                                                              bool recover) noexcept {
    close_segment();

    const std::string path = detail::segment_path(dir_, prefix_, segment_seq);
    const int flags = recover ? O_RDWR : (O_RDWR | O_CREAT | O_EXCL);
    fd_ = ::open(path.c_str(), flags, 0600);
    if (fd_ < 0) {
        return std::unexpected{JournalError::OpenFailed};
    }
    if (!recover && !platform::preallocate(fd_, segment_bytes_).has_value()) {
        return std::unexpected{JournalError::OpenFailed};
    }
    if (recover) {
        // Segment size on disk is authoritative when re-opening.
        struct stat st{};
        if (::fstat(fd_, &st) != 0 || st.st_size < static_cast<off_t>(k_header_bytes)) {
            return std::unexpected{JournalError::OpenFailed};
        }
        segment_bytes_ = static_cast<std::size_t>(st.st_size);
    }

    void* p = ::mmap(nullptr, segment_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        return std::unexpected{JournalError::MapFailed};
    }
    base_ = static_cast<std::byte*>(p);
    segment_seq_ = segment_seq;

    if (recover) {
        header_ = std::launder(reinterpret_cast<FileHeader*>(base_));
        if (header_->magic != k_file_magic) {
            return std::unexpected{JournalError::BadMagic};
        }
        if (header_->abi != k_file_abi) {
            return std::unexpected{JournalError::BadAbi};
        }
        const auto walk = detail::walk_frames(base_, segment_bytes_, header_->base_record_seq);
        write_off_ = walk.end_offset;
        next_seq_ = walk.next_seq;
        header_->committed_bytes.store(write_off_, std::memory_order_release);
        header_->sealed.store(0, std::memory_order_release); // we are appending again
    } else {
        header_ = ::new (base_) FileHeader{};
        header_->magic = k_file_magic;
        header_->abi = k_file_abi;
        header_->segment_seq = segment_seq;
        header_->base_record_seq = next_seq_;
        write_off_ = k_header_bytes;
        header_->committed_bytes.store(write_off_, std::memory_order_release);
    }
    return {};
}

std::expected<std::uint64_t, JournalError> JournalWriter::append(std::uint16_t schema_id,
                                                                 std::uint16_t schema_version,
                                                                 std::span<const std::byte> payload,
                                                                 std::uint64_t ts_ns) noexcept {
    if (payload.size() > k_max_payload) {
        return std::unexpected{JournalError::PayloadTooLarge};
    }
    const std::size_t need = frame_bytes(payload.size());
    if (write_off_ + need > segment_bytes_) {
        header_->sealed.store(1, std::memory_order_release);
        if (auto r = open_segment(segment_seq_ + 1, /*recover=*/false); !r.has_value()) {
            return std::unexpected{JournalError::RotateFailed};
        }
    }

    const std::uint64_t seq = next_seq_;
    std::byte* frame = base_ + write_off_;

    const auto len = static_cast<std::uint32_t>(payload.size());
    std::memcpy(frame, &len, 4);
    std::memcpy(frame + 4, &schema_id, 2);
    std::memcpy(frame + 6, &schema_version, 2);
    std::memcpy(frame + 8, &seq, 8);
    std::memcpy(frame + 16, &ts_ns, 8);
    if (!payload.empty()) {
        std::memcpy(frame + k_frame_header_bytes, payload.data(), payload.size());
    }
    const std::uint32_t crc =
        crc32c::Crc32c(reinterpret_cast<const char*>(frame), k_frame_header_bytes + payload.size());
    std::memcpy(frame + k_frame_header_bytes + payload.size(), &crc, 4);

    write_off_ += need;
    header_->committed_bytes.store(write_off_, std::memory_order_release);
    ++next_seq_;
    return seq;
}

void JournalWriter::flush() noexcept {
    if (fd_ >= 0) {
#if defined(__APPLE__)
        ::fcntl(fd_, F_FULLFSYNC);
#else
        ::fdatasync(fd_);
#endif
    }
}

} // namespace raikiri::journal
