#include <raikiri/audit/day_index.hpp>
#include <raikiri/audit/file_store.hpp>
#include <raikiri/audit/line.hpp>
#include <raikiri/audit/verify.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <ranges>

namespace raikiri::audit {

namespace internal {

constexpr std::uint64_t k_read_bytes = std::uint64_t{4} * 1024 * 1024;

class OpenDay {
public:
    explicit OpenDay(const std::string& path) : fd_{::open(path.c_str(), O_RDONLY | O_CLOEXEC)} {}

    ~OpenDay() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    OpenDay(const OpenDay&) = delete;
    OpenDay& operator=(const OpenDay&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept {
        struct stat info {};

        return fd_ >= 0 && ::fstat(fd_, &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    }

    [[nodiscard]] bool read(std::string& out, std::uint64_t at, std::uint64_t bytes) const {
        out.resize(bytes);
        const auto got = ::pread(fd_, out.data(), bytes, static_cast<off_t>(at));
        if (got < 0) {
            return false;
        }
        out.resize(static_cast<std::size_t>(got));
        return true;
    }

private:
    int fd_;
};

// a wildcard or an unset filter says nothing a bloom filter could use
[[nodiscard]] bool exact(std::string_view wanted) noexcept {
    return !wanted.empty() && wanted.back() != '*';
}

[[nodiscard]] bool may_match(const IndexBlock& block, const ScanHint& hint) noexcept {
    if ((hint.since.has_value() && block.max_ts < hint.since->ns) ||
        (hint.until.has_value() && block.min_ts >= hint.until->ns)) {
        return false;
    }
    return (!exact(hint.actor) || block.may_hold(IndexField::Actor, hint.actor)) &&
           (!exact(hint.action) || block.may_hold(IndexField::Action, hint.action)) &&
           (!exact(hint.subject) || block.may_hold(IndexField::Subject, hint.subject));
}

class BackwardReader {
public:
    BackwardReader(const OpenDay& day, const std::function<bool(const EntryView&)>& visit)
        : day_{&day}, visit_{&visit} {}

    // whole lines inside [from, to), last first; false once told to stop
    [[nodiscard]] bool range(std::uint64_t from, std::uint64_t to) {
        while (to > from) {
            const auto at = to - from > k_read_bytes ? to - k_read_bytes : from;
            if (!day_->read(buffer_, at, to - at)) {
                return true;
            }
            std::string_view text{buffer_};
            // a cut at the front belongs to the next read back; a cut at the back is a line still being written
            auto start = std::string_view::size_type{0};
            if (at != from) {
                start = text.find('\n');
                if (start == std::string_view::npos) {
                    return true;
                }
                ++start;
            }
            if (!lines(text.substr(start))) {
                return false;
            }
            to = at + start;
            if (at == from) {
                break;
            }
        }
        return true;
    }

private:
    [[nodiscard]] bool lines(std::string_view text) {
        const auto last = text.rfind('\n');
        if (last == std::string_view::npos) {
            return true;
        }
        text = text.substr(0, last);
        while (true) {
            const auto stop = text.rfind('\n');
            const auto line = stop == std::string_view::npos ? text : text.substr(stop + 1);
            if (const auto parsed = parse_line(line); parsed.has_value()) {
                unescape_to(actor_, parsed->actor);
                unescape_to(action_, parsed->action);
                unescape_to(subject_, parsed->subject);
                if (!(*visit_)({.seq = parsed->seq,
                                .ts = {parsed->ts},
                                .actor = actor_,
                                .action = action_,
                                .subject = subject_,
                                .detail_json = parsed->detail,
                                .prev = parsed->prev,
                                .line = line})) {
                    return false;
                }
            }
            if (stop == std::string_view::npos) {
                return true;
            }
            text = text.substr(0, stop);
        }
    }

    const OpenDay* day_;
    const std::function<bool(const EntryView&)>* visit_;
    std::string buffer_;
    std::string actor_;
    std::string action_;
    std::string subject_;
};

} // namespace internal

std::vector<std::string> FileStore::streams() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator{root_, ec}) {
        if (item.is_directory(ec)) {
            out.push_back(item.path().filename().string());
        }
    }
    std::ranges::sort(out);
    return out;
}

std::vector<std::uint32_t> FileStore::days(const std::string& stream) const {
    return list_days(root_ + "/" + stream);
}

void FileStore::scan_day(const std::string& stream,
                         std::uint32_t day,
                         const std::function<void(const EntryView&)>& visit) const {
    const auto content = internal::read_file(day_path(root_ + "/" + stream, day));
    if (!content.has_value()) {
        return;
    }
    std::string actor;
    std::string action;
    std::string subject;
    std::string_view rest{*content};
    for (auto stop = rest.find('\n'); stop != std::string_view::npos; stop = rest.find('\n')) {
        const auto line = rest.substr(0, stop);
        rest.remove_prefix(stop + 1);
        const auto parsed = parse_line(line);
        if (!parsed.has_value()) {
            continue;
        }
        unescape_to(actor, parsed->actor);
        unescape_to(action, parsed->action);
        unescape_to(subject, parsed->subject);
        visit({.seq = parsed->seq,
               .ts = {parsed->ts},
               .actor = actor,
               .action = action,
               .subject = subject,
               .detail_json = parsed->detail,
               .prev = parsed->prev,
               .line = line});
    }
}

void FileStore::scan_day_newest(const std::string& stream,
                                std::uint32_t day,
                                const ScanHint& hint,
                                const std::function<bool(const EntryView&)>& visit) const {
    const auto dir = root_ + "/" + stream;
    const internal::OpenDay file{day_path(dir, day)};
    const auto size = file.size();
    if (size == 0) {
        return;
    }
    const auto blocks = read_index(index_path(dir, day), size);
    internal::BackwardReader reader{file, visit};
    auto end = size;
    for (const auto& block : std::ranges::reverse_view(blocks)) {
        const auto block_end = block.offset + block.bytes;
        // what no block covers: a tail still open, or lines from before a crash
        if ((block_end < end && !reader.range(block_end, end)) ||
            (internal::may_match(block, hint) && !reader.range(block.offset, block_end))) {
            return;
        }
        end = block.offset;
    }
    (void)reader.range(0, end);
}

ChainReport FileStore::verify(const std::string& stream, std::uint32_t day) const {
    return verify_day(root_ + "/" + stream, day, &verified_[{stream, day}]);
}

} // namespace raikiri::audit
