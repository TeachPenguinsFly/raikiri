#pragma once

// Postgres sink. Cold plane: exceptions are fine here (two-zone error
// model), and libpqxx stays behind a pimpl so no third-party type appears in
// this header. One commit() = one transaction containing every row of the
// batch PLUS the cursor advance — that single fact turns idempotent inserts
// into exactly-once persistence.

#include <raikiri/persist/rows.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace raikiri::persist {

class PgSink {
public:
    // Throws on connection/prepare failure (startup, loud).
    explicit PgSink(const std::string& dsn);
    PgSink(PgSink&&) noexcept;
    ~PgSink();
    PgSink(const PgSink&) = delete;
    PgSink& operator=(const PgSink&) = delete;
    PgSink& operator=(PgSink&&) = delete;

    // Executes a .sql file (migrations / test setup).
    void exec_file(const std::string& path);

    // Cursor for `journal`; 0 when none exists yet.
    [[nodiscard]] std::uint64_t load_cursor(const std::string& journal);

    // All rows + cursor advance, one transaction. Throws on SQL failure —
    // the caller retries the whole batch (safe: everything is idempotent).
    void commit(const std::string& journal, const RowBatch& batch);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::persist
