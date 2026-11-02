#pragma once

// json lines, appended and fsynced under an flock, because two processes write the same file; losing it is the one way
// an id gets reused, so it sits with the journals and gets the same backup

#include <raikiri/books/registry.hpp>

#include <string>

namespace raikiri::books {

// $RAIKIRI_BOOKS_FILE, else $RAIKIRI_DATA_DIR/books.jsonl; admind and strategyd must land on one file, or an intent
// under a book admind opened is refused
[[nodiscard]] std::string default_path();

class FileBookStore final : public BookStore {
public:
    explicit FileBookStore(std::string path) : path_{std::move(path)} {}

    [[nodiscard]] std::expected<std::vector<Book>, BookError> read() const override;
    [[nodiscard]] std::expected<std::vector<Book>, BookError> update(const Decide& decide) override;

private:
    std::string path_;
};

} // namespace raikiri::books
