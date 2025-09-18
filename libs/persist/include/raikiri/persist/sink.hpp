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

    void commit(const std::string& journal, const RowBatch& batch);

    // How many rows one of our tables holds. Exists so the exactly-once claim
    // is CHECKABLE: a replay that duplicated every row still advances the
    // cursor to the same number, so the cursor alone proves nothing.
    // `table` is an unqualified name in the raikiri schema and is refused
    // unless it is lowercase letters and underscores — it is interpolated,
    // because Postgres takes no parameter in an identifier position.
    [[nodiscard]] std::int64_t count_rows(const std::string& table);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::persist
