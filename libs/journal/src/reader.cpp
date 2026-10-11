#include <raikiri/journal/internal.hpp>
#include <raikiri/journal/reader.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <new>
#include <utility>

namespace raikiri::journal {

JournalReader::JournalReader(JournalReader&& other) noexcept
    : dir_{std::move(other.dir_)}, prefix_{std::move(other.prefix_)}, header_{std::exchange(other.header_, nullptr)},
      base_{std::exchange(other.base_, nullptr)}, map_size_{std::exchange(other.map_size_, 0)},
      segment_seq_{other.segment_seq_}, read_off_{other.read_off_}, last_seq_{other.last_seq_},
      have_segment_{std::exchange(other.have_segment_, false)} {}

JournalReader& JournalReader::operator=(JournalReader&& other) noexcept {
    if (this != &other) {
        close_segment();
        dir_ = std::move(other.dir_);
        prefix_ = std::move(other.prefix_);
        header_ = std::exchange(other.header_, nullptr);
        base_ = std::exchange(other.base_, nullptr);
        map_size_ = std::exchange(other.map_size_, 0);
        segment_seq_ = other.segment_seq_;
        read_off_ = other.read_off_;
        last_seq_ = other.last_seq_;
        have_segment_ = std::exchange(other.have_segment_, false);
    }
    return *this;
}

JournalReader::~JournalReader() {
    close_segment();
}

void JournalReader::close_segment() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, map_size_);
        base_ = nullptr;
        header_ = nullptr;
    }
    have_segment_ = false;
}

std::expected<JournalReader, JournalError> JournalReader::open(std::string dir, std::string prefix) {
    JournalReader r;
    r.dir_ = std::move(dir);
    r.prefix_ = std::move(prefix);
    const auto segments = internal::list_segments(r.dir_, r.prefix_);
    if (segments.empty()) {
        return std::unexpected{JournalError::NoSegments};
    }
    if (!r.open_segment(segments.front())) {
        return std::unexpected{JournalError::OpenFailed};
    }
    return r;
}

std::expected<JournalReader, JournalError>
JournalReader::open_after(std::string dir, std::string prefix, std::uint64_t after_seq) {
    auto r = open(std::move(dir), std::move(prefix));
    if (!r.has_value()) {
        return r;
    }
    // Skip forward; segments' base_record_seq lets us jump whole files.
    FrameView frame{};
    while (r->last_seq_ < after_seq) {
        const auto result = r->next(frame);
        if (result != ReadResult::Item) {
            break; // journal ends before the cursor — caller resumes from here
        }
    }
    return r;
}

bool JournalReader::open_segment(std::uint64_t segment_seq) noexcept {
    close_segment();
    const std::string path = internal::segment_path(dir_, prefix_, segment_seq);
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st {};

    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(k_header_bytes)) {
        ::close(fd);
        return false;
    }
    void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        return false;
    }
    base_ = static_cast<std::byte*>(p);
    map_size_ = static_cast<std::size_t>(st.st_size);
    header_ = std::launder(reinterpret_cast<const FileHeader*>(base_));
    if (header_->magic != k_file_magic || header_->abi != k_file_abi) {
        close_segment();
        return false;
    }
    segment_seq_ = segment_seq;
    read_off_ = k_header_bytes;
    have_segment_ = true;
    return true;
}

ReadResult JournalReader::next(FrameView& out) noexcept {
    if (!have_segment_) {
        const auto segments = internal::list_segments(dir_, prefix_);
        if (segments.empty() || !open_segment(segments.front())) {
            return ReadResult::Empty;
        }
    }

    const auto committed = header_->committed_bytes.load(std::memory_order_acquire);
    if (read_off_ >= committed) {
        if (header_->sealed.load(std::memory_order_acquire) != 0 && open_segment(segment_seq_ + 1)) {
            return next(out);
        }
        return ReadResult::Empty;
    }

    if (!internal::decode_frame(base_, map_size_, read_off_, out)) {
        return ReadResult::Corrupt;
    }
    read_off_ += frame_bytes(out.payload.size());
    last_seq_ = out.seq;
    return ReadResult::Item;
}

} // namespace raikiri::journal
