#pragma once

// one commit() is one transaction holding every row plus the cursor advance

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

    // `table` is interpolated, so only lowercase letters and underscores are accepted
    [[nodiscard]] std::int64_t count_rows(const std::string& table);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::persist
