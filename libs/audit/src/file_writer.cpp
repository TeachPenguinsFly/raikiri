#include <raikiri/audit/file_writer.hpp>
#include <raikiri/audit/line.hpp>
#include <raikiri/audit/verify.hpp>
#include <raikiri/log/log.hpp>

#include <fmt/format.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <optional>
#include <utility>

namespace raikiri::audit {

namespace internal {

constexpr std::size_t k_flush_bytes = std::size_t{64} * 1024;

struct Tail {
    std::uint64_t kept_bytes{0};
    std::uint64_t torn_bytes{0};
    Digest torn_hash{};
    std::optional<Tip> tip{};
    bool unreadable_tail{false};
};

// `content` ends at the last newline once torn bytes are cut; a line that will not parse still chains, by counting
// lines back to the last one that will
[[nodiscard]] Tail read_tail(std::string_view content) {
    Tail out{};
    const auto last_newline = content.rfind('\n');
    const std::size_t kept = last_newline == std::string_view::npos ? 0 : last_newline + 1;
    out.kept_bytes = kept;
    out.torn_bytes = content.size() - kept;
    if (out.torn_bytes != 0) {
        out.torn_hash = sha256(content.substr(kept));
    }
    auto whole = content.substr(0, kept);
    std::optional<Digest> last_hash;
    std::uint64_t unreadable = 0;
    while (!whole.empty()) {
        whole.remove_suffix(1);
        const auto before = whole.rfind('\n');
        const auto line = before == std::string_view::npos ? whole : whole.substr(before + 1);
        if (!last_hash.has_value()) {
            last_hash = sha256(line);
        }
        if (const auto parsed = parse_line(line); parsed.has_value()) {
            out.tip = Tip{.seq = parsed->seq + unreadable, .hash = *last_hash};
            break;
        }
        ++unreadable;
        whole = before == std::string_view::npos ? std::string_view{} : whole.substr(0, before + 1);
    }
    if (!out.tip.has_value() && last_hash.has_value()) {
        out.tip = Tip{.seq = unreadable - 1, .hash = *last_hash};
    }
    out.unreadable_tail = unreadable != 0;
    return out;
}

} // namespace internal

std::string_view to_string(WriterError error) noexcept {
    switch (error) {
    case WriterError::OpenFailed:
        return "open failed";
    case WriterError::AlreadyWriting:
        return "another process writes this stream";
    case WriterError::RecoveryFailed:
        return "recovery failed";
    }
    return "open failed";
}

FileWriter::FileWriter(std::string stream_dir, FsyncPolicy fsync) noexcept
    : stream_dir_{std::move(stream_dir)}, fsync_{fsync} {}

FileWriter::FileWriter(FileWriter&& other) noexcept
    : stream_dir_{std::move(other.stream_dir_)}, fsync_{other.fsync_}, lock_fd_{std::exchange(other.lock_fd_, -1)},
      head_fd_{std::exchange(other.head_fd_, -1)}, day_fd_{std::exchange(other.day_fd_, -1)}, day_{other.day_},
      file_bytes_{other.file_bytes_}, pending_{std::move(other.pending_)}, next_seq_{other.next_seq_},
      prev_{other.prev_}, stats_{other.stats_} {}

FileWriter::~FileWriter() {
    if (day_fd_ >= 0) {
        if (!flush()) {
            RLOG_ERROR("audit: {} lost {} bytes at close", stream_dir_, pending_.size());
        }
        sync();
    }
    close_all();
}

void FileWriter::close_all() noexcept {
    for (int* fd : {&day_fd_, &head_fd_, &lock_fd_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
}

std::expected<FileWriter, WriterError>
FileWriter::open(const std::string& stream_dir, FsyncPolicy fsync, const Clock& clock) {
    std::error_code ec;
    std::filesystem::create_directories(stream_dir, ec);
    if (ec) {
        return std::unexpected{WriterError::OpenFailed};
    }
    FileWriter writer{stream_dir, fsync};
    writer.lock_fd_ = ::open((stream_dir + "/LOCK").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0640);
    if (writer.lock_fd_ < 0) {
        return std::unexpected{WriterError::OpenFailed};
    }
    if (::flock(writer.lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        return std::unexpected{WriterError::AlreadyWriting};
    }
    writer.head_fd_ = ::open(head_path(stream_dir).c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0640);
    if (writer.head_fd_ < 0) {
        return std::unexpected{WriterError::OpenFailed};
    }
    if (!writer.recover(clock)) {
        return std::unexpected{WriterError::RecoveryFailed};
    }
    return writer;
}

bool FileWriter::recover(const Clock& clock) {
    const auto head = read_head(stream_dir_);
    const auto days = list_days(stream_dir_);
    std::uint64_t torn_bytes = 0;
    Digest torn_hash{};
    bool unreadable_tail = false;
    std::optional<Tip> tip;
    bool newest = true;
    for (auto day = days.rbegin(); day != days.rend() && !tip.has_value(); ++day, newest = false) {
        const auto path = day_path(stream_dir_, *day);
        const auto content = internal::read_file(path);
        if (!content.has_value()) {
            return false;
        }
        const auto tail = internal::read_tail(*content);
        if (newest) {
            day_ = *day;
            torn_bytes = tail.torn_bytes;
            torn_hash = tail.torn_hash;
            unreadable_tail = tail.unreadable_tail;
            if (tail.torn_bytes != 0 && ::truncate(path.c_str(), static_cast<off_t>(tail.kept_bytes)) != 0) {
                return false;
            }
        }
        tip = tail.tip;
    }
    if (tip.has_value()) {
        next_seq_ = tip->seq + 1;
        prev_ = tip->hash;
    }
    if (!days.empty() && !roll_to(day_)) {
        return false;
    }

    const bool head_ahead = head.has_value() && (!tip.has_value() || head->seq > tip->seq);
    const bool head_differs = head.has_value() && tip.has_value() && head->seq == tip->seq && head->hash != tip->hash;
    if (torn_bytes == 0 && !head_ahead && !head_differs && !unreadable_tail) {
        return true;
    }
    const auto detail = fmt::format(R"({{"torn_bytes":{},"torn_sha256":"{}","head_ahead":{},"head_differs":{},)"
                                    R"("unreadable_tail":{},"head_seq":"{}","next_seq":"{}"}})",
                                    torn_bytes,
                                    torn_bytes == 0 ? std::string{} : to_hex(torn_hash),
                                    head_ahead,
                                    head_differs,
                                    unreadable_tail,
                                    head.has_value() ? head->seq : 0,
                                    next_seq_);
    RLOG_WARN("audit: {} recovered: {}", stream_dir_, detail);
    const auto subject = std::filesystem::path{stream_dir_}.filename().string();
    return append({.ts = clock.wall(),
                   .actor = "audit",
                   .action = k_action_recovery,
                   .subject = subject,
                   .detail_json = detail}) &&
           flush();
}

bool FileWriter::roll_to(std::uint32_t day) {
    if (day_fd_ >= 0) {
        if (!flush()) {
            return false;
        }
        sync();
    }
    const auto path = day_path(stream_dir_, day);
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0) {
        RLOG_ERROR("audit: cannot open {}", path);
        ++stats_.write_failures;
        return false;
    }

    struct stat info {};

    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        ++stats_.write_failures;
        return false;
    }
    if (day_fd_ >= 0) {
        ::close(day_fd_);
        ++stats_.rotations;
    }
    day_fd_ = fd;
    day_ = day;
    file_bytes_ = static_cast<std::uint64_t>(info.st_size);
    return true;
}

bool FileWriter::append(const Entry& entry) {
    // never back a day: a clock stepped back must not reopen a finished file
    const auto day = std::max(day_of(entry.ts), day_);
    if ((day_fd_ < 0 || day != day_) && !roll_to(day)) {
        return false;
    }
    const auto start = pending_.size();
    if (!append_line(pending_, next_seq_, entry, prev_)) {
        ++stats_.unparsed_details;
    }
    prev_ = sha256(std::string_view{pending_}.substr(start));
    pending_.push_back('\n');
    ++next_seq_;
    ++stats_.lines;
    if (fsync_ == FsyncPolicy::EveryEntry || pending_.size() >= internal::k_flush_bytes) {
        // a failure keeps the line in pending_, and the caller's next flush sees it
        (void)flush();
    }
    return true;
}

bool FileWriter::flush() {
    if (pending_.empty()) {
        return true;
    }
    std::string_view rest{pending_};
    while (!rest.empty()) {
        const auto wrote = ::write(day_fd_, rest.data(), rest.size());
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            ++stats_.write_failures;
            if (::ftruncate(day_fd_, static_cast<off_t>(file_bytes_)) != 0) {
                RLOG_ERROR("audit: {} cannot cut a short write back", stream_dir_);
            }
            return false;
        }
        rest.remove_prefix(static_cast<std::size_t>(wrote));
    }
    file_bytes_ += pending_.size();
    pending_.clear();
    if (fsync_ != FsyncPolicy::Never) {
        sync();
    }
    write_head();
    return true;
}

void FileWriter::sync() noexcept {
    if (day_fd_ < 0) {
        return;
    }
#if defined(__APPLE__)
    const int rc = ::fcntl(day_fd_, F_FULLFSYNC);
#else
    const int rc = ::fdatasync(day_fd_);
#endif
    ++stats_.syncs;
    if (rc != 0) {
        ++stats_.write_failures;
    }
}

void FileWriter::write_head() noexcept {
    // seq digits only grow, so this overwrites whatever is there
    std::array<char, 96> buf{};
    const auto hex = to_hex(prev_);
    const auto done = fmt::format_to_n(buf.data(), buf.size(), "{} {}\n", next_seq_ - 1, hex);
    if (::pwrite(head_fd_, buf.data(), done.size, 0) != static_cast<ssize_t>(done.size)) {
        ++stats_.write_failures;
    }
}

} // namespace raikiri::audit
