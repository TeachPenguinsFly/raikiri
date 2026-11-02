#include <raikiri/books/registry.hpp>

#include <algorithm>

namespace raikiri::books {
namespace internal {

[[nodiscard]] const Book* named(const std::vector<Book>& books, std::string_view name) noexcept {
    const auto found = std::ranges::find(books, name, &Book::name);
    return found == books.end() ? nullptr : &*found;
}

[[nodiscard]] Book shared_manual(UnixNanos now) {
    return {.id = k_manual_strategy_id, .kind = BookKind::Manual, .name = std::string{k_shared_manual}, .created = now};
}

} // namespace internal

std::expected<std::vector<Book>, BookError> MemoryBookStore::update(const Decide& decide) {
    for (auto& book : decide(books_)) {
        books_.push_back(std::move(book));
    }
    return books_;
}

Registry::Registry(BookStore& store, std::function<UnixNanos()> wall) : store_{&store}, wall_{std::move(wall)} {
    index({internal::shared_manual({})});
}

void Registry::index(std::vector<Book> books) {
    books_ = std::move(books);
    by_id_.clear();
    by_name_.clear();
    for (std::size_t i = 0; i < books_.size(); ++i) {
        by_id_.emplace(books_[i].id, i);
        // two strategies may share a name; the first holds it and the rest answer to their number
        by_name_.emplace(books_[i].name, i);
    }
}

std::expected<void, BookError> Registry::load() {
    auto all = store_->update([this](const std::vector<Book>& on_record) -> std::vector<Book> {
        if (std::ranges::find(on_record, k_manual_strategy_id, &Book::id) != on_record.end()) {
            return {};
        }
        return {internal::shared_manual(wall_())};
    });
    if (!all.has_value()) {
        return std::unexpected{all.error()};
    }
    index(std::move(*all));
    return {};
}

void Registry::refresh() {
    auto all = store_->read();
    if (all.has_value() && !all->empty() && all->size() != books_.size()) {
        index(std::move(*all));
    }
}

const Book* Registry::find(std::uint16_t id) const noexcept {
    const auto found = by_id_.find(id);
    return found == by_id_.end() ? nullptr : &books_[found->second];
}

const Book* Registry::find(std::string_view name) const noexcept {
    const auto found = by_name_.find(std::string{name});
    return found == by_name_.end() ? nullptr : &books_[found->second];
}

BookKind Registry::kind_of(std::uint16_t id) const noexcept {
    const auto* book = find(id);
    return book == nullptr ? BookKind::Unknown : book->kind;
}

std::vector<Book> Registry::list() const {
    return books_;
}

std::expected<Book, BookError> Registry::open(std::string_view name) {
    const auto kind = reserved_kind(name);
    if (kind == BookKind::Unknown) {
        return std::unexpected{BookError::BadName};
    }
    if (const auto* held = find(name); held != nullptr) {
        return *held;
    }
    auto refused = BookError::Store;
    auto all = store_->update([&](const std::vector<Book>& on_record) -> std::vector<Book> {
        if (internal::named(on_record, name) != nullptr) {
            return {}; // the other process got here first
        }
        const auto id = next_id(on_record);
        if (!id.has_value()) {
            refused = id.error();
            return {};
        }
        return {Book{.id = *id, .kind = kind, .name = std::string{name}, .created = wall_()}};
    });
    if (!all.has_value()) {
        return std::unexpected{all.error()};
    }
    index(std::move(*all));
    const auto* made = find(name);
    if (made == nullptr) {
        return std::unexpected{refused};
    }
    return *made;
}

std::expected<Book, BookError> Registry::resolve(std::string_view name) {
    if (const auto* held = find(name); held != nullptr) {
        return *held;
    }
    refresh();
    if (const auto* held = find(name); held != nullptr) {
        return *held;
    }
    switch (reserved_kind(name)) {
    case BookKind::Manual:
    case BookKind::Hedge:
        return open(name);
    default:
        return std::unexpected{BookError::NotFound};
    }
}

std::expected<Book, BookError> Registry::register_strategy(std::uint16_t id, std::string_view name) {
    if (id == 0 || name.empty() || reserved_kind(name) != BookKind::Unknown || name.starts_with(k_manual_prefix) ||
        name.starts_with(k_client_prefix)) {
        return std::unexpected{BookError::BadName};
    }
    auto refused = BookError::Store;
    auto all = store_->update([&](const std::vector<Book>& on_record) -> std::vector<Book> {
        const auto held = std::ranges::find(on_record, id, &Book::id);
        if (held == on_record.end()) {
            return {Book{.id = id, .kind = BookKind::Strategy, .name = std::string{name}, .created = wall_()}};
        }
        if (held->kind != BookKind::Strategy || held->name != name) {
            refused = BookError::IdTaken;
        }
        return {};
    });
    if (!all.has_value()) {
        return std::unexpected{all.error()};
    }
    index(std::move(*all));
    if (refused == BookError::IdTaken) {
        return std::unexpected{refused};
    }
    return *find(id);
}

bool Registry::takes_intents(ClientOrderId id) {
    if (id.is_child()) {
        return false; // an executor's child is answered for through its parent
    }
    if (find(id.strategy_id()) == nullptr) {
        refresh();
    }
    const auto kind = kind_of(id.strategy_id());
    return kind == BookKind::Manual || kind == BookKind::Client;
}

} // namespace raikiri::books
