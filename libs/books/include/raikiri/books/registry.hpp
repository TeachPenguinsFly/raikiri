#pragma once

// admind and strategyd each hold one over the same store: admind opens a book for a login or a client, and strategyd
// meets the id on its intent ring; single-threaded within a process

#include <raikiri/books/book.hpp>

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::books {

class BookStore {
public:
    BookStore() = default;
    BookStore(const BookStore&) = delete;
    BookStore& operator=(const BookStore&) = delete;

    [[nodiscard]] virtual std::expected<std::vector<Book>, BookError> read() const = 0;

    // `decide` sees every book on record with other writers held out; what it returns is durable before this returns
    using Decide = std::function<std::vector<Book>(const std::vector<Book>&)>;
    [[nodiscard]] virtual std::expected<std::vector<Book>, BookError> update(const Decide& decide) = 0;

protected:
    ~BookStore() = default;
};

class MemoryBookStore final : public BookStore {
public:
    [[nodiscard]] std::expected<std::vector<Book>, BookError> read() const override { return books_; }

    [[nodiscard]] std::expected<std::vector<Book>, BookError> update(const Decide& decide) override;

private:
    std::vector<Book> books_;
};

class Registry {
public:
    Registry(BookStore& store, std::function<UnixNanos()> wall);

    // records the shared manual book on a first run; a failure leaves only that book, in memory
    [[nodiscard]] std::expected<void, BookError> load();

    // what another process recorded since the last look
    void refresh();

    [[nodiscard]] const Book* find(std::uint16_t id) const noexcept;
    [[nodiscard]] const Book* find(std::string_view name) const noexcept;

    // Unknown for an id with nothing on record, which is any order older than the registry
    [[nodiscard]] BookKind kind_of(std::uint16_t id) const noexcept;

    [[nodiscard]] std::vector<Book> list() const;

    // `manual:<login>`, `client:<name>`, `hedge` or `desk`, made if missing
    [[nodiscard]] std::expected<Book, BookError> open(std::string_view name);

    // the book a named order goes to: `manual:<login>` and `hedge` are made on first use, a client must have been
    // opened already, so a typo cannot start a client's book
    [[nodiscard]] std::expected<Book, BookError> resolve(std::string_view name);

    // a known id under another name is refused: its history belongs to that other strategy
    [[nodiscard]] std::expected<Book, BookError> register_strategy(std::uint16_t id, std::string_view name);

    // whether strategyd takes an intent minted under this id; a book opened a moment ago is read off the store
    [[nodiscard]] bool takes_intents(ClientOrderId id);

private:
    void index(std::vector<Book> books);

    BookStore* store_;
    std::function<UnixNanos()> wall_;
    std::vector<Book> books_;
    std::unordered_map<std::uint16_t, std::size_t> by_id_;
    std::unordered_map<std::string, std::size_t> by_name_;
};

} // namespace raikiri::books
