#pragma once

// a few nonblocking postgres connections on one IoContext: a query waits for a free connection, runs there, and its
// callback gets the whole result on the loop's thread. Results come back as text; a query that wants json can build it
// in sql (json_agg, row_to_json) and pass it through. libpq stays behind this header

#include <raikiri/net/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct pg_result;

namespace raikiri::pg {

struct Settings {
    // a libpq connection string or uri
    std::string dsn;
    std::size_t connections{4};
    // set on every connection; postgres cancels a statement that runs longer
    std::chrono::milliseconds statement_timeout{10'000};
    // a query beyond this many waiting fails at once rather than queue without bound
    std::size_t max_queued{1024};
    // the wait before connecting again after a refusal or a lost connection, doubled up to the cap
    std::chrono::milliseconds reconnect_first{500};
    std::chrono::milliseconds reconnect_cap{30'000};
};

class Result {
public:
    Result() = default;
    static Result failure(std::string error);
    static Result of(pg_result* owned);

    // false when postgres refused the statement or no connection could run it
    [[nodiscard]] bool ok() const noexcept { return error_.empty(); }

    [[nodiscard]] std::string_view error() const noexcept { return error_; }

    [[nodiscard]] std::size_t rows() const noexcept;
    [[nodiscard]] std::size_t columns() const noexcept;
    [[nodiscard]] std::string_view name(std::size_t column) const noexcept;
    [[nodiscard]] std::optional<std::size_t> column(std::string_view name) const noexcept;
    // a null reads as empty; null() tells the two apart
    [[nodiscard]] std::string_view value(std::size_t row, std::size_t column) const noexcept;
    [[nodiscard]] bool null(std::size_t row, std::size_t column) const noexcept;
    // rows changed by an insert, update or delete
    [[nodiscard]] std::uint64_t affected() const noexcept;

private:
    std::shared_ptr<pg_result> result_;
    std::string error_;
};

// $1, $2, ... in the sql; nullopt is null
using Params = std::vector<std::optional<std::string>>;
using Done = std::function<void(Result)>;

struct Stats {
    std::size_t connected{0};
    std::size_t busy{0};
    std::size_t queued{0};
    std::uint64_t completed{0};
    std::uint64_t failed{0};
};

class Pool {
public:
    // starts connecting at once; queries made before a connection is up wait for it
    Pool(net::IoContext& io, Settings settings);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // `done` runs once, on the loop's thread, never inside this call
    void query(std::string sql, Params params, Done done);

    void query(std::string sql, Done done) { query(std::move(sql), {}, std::move(done)); }

    [[nodiscard]] Stats stats() const noexcept;

    struct State;

private:
    std::shared_ptr<State> state_;
};

} // namespace raikiri::pg
