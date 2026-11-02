#include <raikiri/books/file_store.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <glaze/glaze.hpp>
#include <string_view>

namespace raikiri::books {

namespace dto {

struct Line {
    std::uint16_t id{0};
    std::string kind{};
    std::string name{};
    std::uint64_t created{0};
};

} // namespace dto

namespace internal {

class Fd {
public:
    explicit Fd(int fd) noexcept : fd_{fd} {}

    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

[[nodiscard]] bool slurp(int fd, std::string& out) {
    if (::lseek(fd, 0, SEEK_SET) != 0) {
        return false;
    }
    char buf[4096];
    for (;;) {
        const auto got = ::read(fd, buf, sizeof buf);
        if (got < 0) {
            return false;
        }
        if (got == 0) {
            return true;
        }
        out.append(buf, static_cast<std::size_t>(got));
    }
}

// a line with no newline is an append a crash cut short: it was never acknowledged, so it never happened. any other
// bad line refuses the file, since reading past it could hand out its id again
[[nodiscard]] std::expected<std::vector<Book>, BookError> parse(std::string_view text) {
    std::vector<Book> out;
    while (!text.empty()) {
        const auto end = text.find('\n');
        if (end == std::string_view::npos) {
            break;
        }
        dto::Line line{};
        const auto body = std::string{text.substr(0, end)};
        text.remove_prefix(end + 1);
        if (body.empty()) {
            continue;
        }
        const auto kind = glz::read_json(line, body) ? BookKind::Unknown : book_kind_from_string(line.kind);
        if (kind == BookKind::Unknown || line.id == 0) {
            return std::unexpected{BookError::Store};
        }
        out.push_back({.id = line.id, .kind = kind, .name = std::move(line.name), .created = UnixNanos{line.created}});
    }
    return out;
}

[[nodiscard]] bool write_all(int fd, std::string_view text) {
    while (!text.empty()) {
        const auto wrote = ::write(fd, text.data(), text.size());
        if (wrote <= 0) {
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(wrote));
    }
    return true;
}

} // namespace internal

std::string default_path() {
    if (const char* file = std::getenv("RAIKIRI_BOOKS_FILE"); file != nullptr && *file != '\0') {
        return file;
    }
    const char* data_dir = std::getenv("RAIKIRI_DATA_DIR");
    return std::string{data_dir != nullptr ? data_dir : "raikiri-data"} + "/books.jsonl";
}

std::expected<std::vector<Book>, BookError> FileBookStore::read() const {
    const internal::Fd fd{::open(path_.c_str(), O_RDONLY | O_CLOEXEC)};
    if (fd.get() < 0) {
        if (errno == ENOENT) {
            return std::vector<Book>{};
        }
        return std::unexpected{BookError::Store};
    }
    std::string text;
    if (!internal::slurp(fd.get(), text)) {
        return std::unexpected{BookError::Store};
    }
    return internal::parse(text);
}

std::expected<std::vector<Book>, BookError> FileBookStore::update(const Decide& decide) {
    std::error_code ec;
    const auto parent = std::filesystem::path{path_}.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    const internal::Fd fd{::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
    if (fd.get() < 0 || ::flock(fd.get(), LOCK_EX) != 0) {
        return std::unexpected{BookError::Store};
    }
    std::string text;
    if (!internal::slurp(fd.get(), text)) {
        return std::unexpected{BookError::Store};
    }
    auto all = internal::parse(text);
    if (!all.has_value()) {
        return all;
    }
    auto fresh = decide(*all);
    if (fresh.empty()) {
        return all;
    }
    // drop a line a crash cut short, or the next append would join it into a bad one
    if (!text.empty() && text.back() != '\n') {
        const auto keep = text.rfind('\n');
        if (::ftruncate(fd.get(), keep == std::string::npos ? 0 : static_cast<off_t>(keep + 1)) != 0) {
            return std::unexpected{BookError::Store};
        }
    }
    std::string lines;
    for (const auto& book : fresh) {
        const dto::Line line{
            .id = book.id, .kind = std::string{to_string(book.kind)}, .name = book.name, .created = book.created.ns};
        std::string json;
        if (glz::write_json(line, json)) {
            return std::unexpected{BookError::Store};
        }
        lines += json;
        lines += '\n';
    }
    if (!internal::write_all(fd.get(), lines) || ::fsync(fd.get()) != 0) {
        return std::unexpected{BookError::Store};
    }
    for (auto& book : fresh) {
        all->push_back(std::move(book));
    }
    return all;
}

} // namespace raikiri::books
