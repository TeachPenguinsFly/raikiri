#include <raikiri/pg/pool.hpp>

#include <libpq-fe.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <charconv>
#include <deque>
#include <format>
#include <utility>

namespace raikiri::pg {

namespace {

namespace asio = boost::asio;
using Wait = asio::posix::stream_descriptor::wait_type;

std::string trimmed(const char* message) {
    std::string_view text{message == nullptr ? "" : message};
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    return std::string{text};
}

struct Job {
    std::string sql;
    Params params;
    Done done;
};

void deliver(const Done& done, Result result) noexcept {
    try {
        done(std::move(result));
    } catch (...) {
        // a callback's own failure is not the pool's; the next query still runs
    }
}

class Connection;

} // namespace

Result Result::failure(std::string error) {
    Result r;
    r.error_ = error.empty() ? std::string{"query failed"} : std::move(error);
    return r;
}

Result Result::of(pg_result* owned) {
    Result r;
    r.result_ = std::shared_ptr<pg_result>{owned, PQclear};
    const auto status = PQresultStatus(owned);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        r.error_ = trimmed(PQresultErrorMessage(owned));
        if (r.error_.empty()) {
            r.error_ = PQresStatus(status);
        }
    }
    return r;
}

std::size_t Result::rows() const noexcept {
    return result_ ? static_cast<std::size_t>(PQntuples(result_.get())) : 0;
}

std::size_t Result::columns() const noexcept {
    return result_ ? static_cast<std::size_t>(PQnfields(result_.get())) : 0;
}

std::string_view Result::name(std::size_t column) const noexcept {
    if (column >= columns()) {
        return {};
    }
    return PQfname(result_.get(), static_cast<int>(column));
}

std::optional<std::size_t> Result::column(std::string_view wanted) const noexcept {
    for (std::size_t c = 0; c < columns(); ++c) {
        if (name(c) == wanted) {
            return c;
        }
    }
    return std::nullopt;
}

std::string_view Result::value(std::size_t row, std::size_t column) const noexcept {
    if (row >= rows() || column >= columns()) {
        return {};
    }
    const auto r = static_cast<int>(row);
    const auto c = static_cast<int>(column);
    return {PQgetvalue(result_.get(), r, c), static_cast<std::size_t>(PQgetlength(result_.get(), r, c))};
}

bool Result::null(std::size_t row, std::size_t column) const noexcept {
    if (row >= rows() || column >= columns()) {
        return true;
    }
    return PQgetisnull(result_.get(), static_cast<int>(row), static_cast<int>(column)) == 1;
}

std::uint64_t Result::affected() const noexcept {
    if (!result_) {
        return 0;
    }
    const std::string_view text = PQcmdTuples(result_.get());
    std::uint64_t n = 0;
    std::from_chars(text.data(), text.data() + text.size(), n);
    return n;
}

struct Pool::State {
    State(asio::io_context& i, Settings s) : io{i}, settings{std::move(s)} {}

    void dispatch();
    // nothing can run the waiting queries while every connection is down
    void fail_queued(const std::string& error);
    [[nodiscard]] bool any_up() const noexcept;

    asio::io_context& io;
    Settings settings;
    std::deque<Job> queue;
    std::vector<std::shared_ptr<Connection>> connections;
    std::uint64_t completed{0};
    std::uint64_t failed{0};
    std::string last_error;
    bool closed{false};
};

namespace {

// one libpq connection driven by readiness on its socket; every step runs on the loop's thread
class Connection : public std::enable_shared_from_this<Connection> {
public:
    enum class Phase : std::uint8_t { Connecting, Ready, Busy, Down, Closed };

    explicit Connection(std::shared_ptr<Pool::State> state)
        : state_{std::move(state)}, socket_{state_->io}, timer_{state_->io},
          backoff_{state_->settings.reconnect_first} {}

    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    void connect() {
        const auto options = std::format("-c statement_timeout={}", state_->settings.statement_timeout.count());
        const char* keys[] = {"dbname", "options", nullptr};
        const char* values[] = {state_->settings.dsn.c_str(), options.c_str(), nullptr};
        conn_ = PQconnectStartParams(keys, values, 1);
        if (conn_ == nullptr || PQstatus(conn_) == CONNECTION_BAD) {
            refused();
            return;
        }
        PQsetnonblocking(conn_, 1);
        phase_ = Phase::Connecting;
        // libpq's contract: start as if PQconnectPoll had asked to write
        connect_step(PGRES_POLLING_WRITING);
    }

    void run(Job job) {
        job_ = std::move(job);
        phase_ = Phase::Busy;
        got_ = Result{};
        have_ = false;
        std::vector<const char*> values;
        values.reserve(job_->params.size());
        for (const auto& p : job_->params) {
            values.push_back(p ? p->c_str() : nullptr);
        }
        if (PQsendQueryParams(conn_,
                              job_->sql.c_str(),
                              static_cast<int>(values.size()),
                              nullptr,
                              values.data(),
                              nullptr,
                              nullptr,
                              0) == 0) {
            lost(trimmed(PQerrorMessage(conn_)));
            return;
        }
        flush();
    }

    void shutdown() noexcept {
        phase_ = Phase::Closed;
        job_.reset();
        timer_.cancel();
        close();
    }

private:
    void connect_step(PostgresPollingStatusType want) {
        // libpq may open a new socket between steps (another host, ssl refused), so bind afresh each time
        release();
        const int fd = PQsocket(conn_);
        if (fd < 0) {
            refused();
            return;
        }
        boost::system::error_code ec;
        if (socket_.assign(fd, ec)) {
            refused();
            return;
        }
        wait(want == PGRES_POLLING_READING ? Wait::wait_read : Wait::wait_write, &Connection::connect_ready);
    }

    void connect_ready() {
        switch (const auto status = PQconnectPoll(conn_)) {
        case PGRES_POLLING_READING:
        case PGRES_POLLING_WRITING:
            connect_step(status);
            return;
        case PGRES_POLLING_OK:
            phase_ = Phase::Ready;
            backoff_ = state_->settings.reconnect_first;
            state_->dispatch();
            return;
        default:
            refused();
            return;
        }
    }

    void flush() {
        const int left = PQflush(conn_);
        if (left < 0) {
            lost(trimmed(PQerrorMessage(conn_)));
        } else if (left == 1) {
            wait(Wait::wait_write, &Connection::flush);
        } else {
            wait(Wait::wait_read, &Connection::readable);
        }
    }

    void readable() {
        if (PQconsumeInput(conn_) == 0) {
            lost(trimmed(PQerrorMessage(conn_)));
            return;
        }
        while (PQisBusy(conn_) == 0) {
            pg_result* next = PQgetResult(conn_);
            if (next == nullptr) {
                complete();
                return;
            }
            auto result = Result::of(next);
            // the first refusal is the answer; otherwise the last result is
            if (!have_ || got_.ok()) {
                got_ = std::move(result);
                have_ = true;
            }
        }
        wait(Wait::wait_read, &Connection::readable);
    }

    void complete() {
        if (!job_) {
            return;
        }
        auto job = std::move(*job_);
        job_.reset();
        phase_ = Phase::Ready;
        auto result = have_ ? std::move(got_) : Result::failure("no result");
        ++(result.ok() ? state_->completed : state_->failed);
        // the state may run this connection's next job from inside the callback
        auto keep = shared_from_this();
        deliver(job.done, std::move(result));
        state_->dispatch();
    }

    void wait(Wait type, void (Connection::*next)()) {
        socket_.async_wait(type, [self = shared_from_this(), next](const boost::system::error_code& ec) {
            if (self->phase_ == Phase::Closed || ec == asio::error::operation_aborted) {
                return;
            }
            if (ec) {
                self->lost(ec.message());
                return;
            }
            ((*self).*next)();
        });
    }

    // the connection went away under a query: that query fails, and the rest wait while it connects again at once;
    // only a refusal waits before the next try
    void lost(std::string error) {
        state_->last_error = error;
        auto job = std::exchange(job_, std::nullopt);
        close();
        phase_ = Phase::Down;
        if (job) {
            ++state_->failed;
            deliver(job->done, Result::failure(std::move(error)));
        }
        if (phase_ == Phase::Down) {
            connect();
        }
    }

    void refused() {
        state_->last_error = conn_ == nullptr ? std::string{"out of memory"} : trimmed(PQerrorMessage(conn_));
        down();
        if (!state_->any_up()) {
            state_->fail_queued(state_->last_error);
        }
    }

    void down() {
        close();
        phase_ = Phase::Down;
        timer_.expires_after(backoff_);
        backoff_ = std::min(backoff_ * 2, state_->settings.reconnect_cap);
        timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec && self->phase_ == Phase::Down) {
                self->connect();
            }
        });
    }

    void release() noexcept {
        if (socket_.is_open()) {
            boost::system::error_code ec;
            (void)socket_.cancel(ec);
            (void)socket_.release(); // the descriptor is libpq's to close
        }
    }

    void close() noexcept {
        release();
        if (conn_ != nullptr) {
            PQfinish(conn_);
            conn_ = nullptr;
        }
    }

    std::shared_ptr<Pool::State> state_;
    PGconn* conn_{nullptr};
    asio::posix::stream_descriptor socket_;
    asio::steady_timer timer_;
    std::chrono::milliseconds backoff_;
    Phase phase_{Phase::Down};
    std::optional<Job> job_;
    Result got_;
    bool have_{false};
};

} // namespace

void Pool::State::dispatch() {
    for (const auto& c : connections) {
        if (queue.empty() || closed) {
            return;
        }
        if (c->phase() == Connection::Phase::Ready) {
            auto job = std::move(queue.front());
            queue.pop_front();
            c->run(std::move(job));
        }
    }
}

void Pool::State::fail_queued(const std::string& error) {
    auto waiting = std::exchange(queue, {});
    for (auto& job : waiting) {
        ++failed;
        deliver(job.done, Result::failure(error));
    }
}

bool Pool::State::any_up() const noexcept {
    return std::ranges::any_of(connections, [](const auto& c) {
        return c->phase() == Connection::Phase::Ready || c->phase() == Connection::Phase::Busy ||
               c->phase() == Connection::Phase::Connecting;
    });
}

Pool::Pool(net::IoContext& io, Settings settings)
    : state_{std::make_shared<State>(*static_cast<asio::io_context*>(io.native()), std::move(settings))} {
    const auto n = std::max<std::size_t>(1, state_->settings.connections);
    for (std::size_t i = 0; i < n; ++i) {
        state_->connections.push_back(std::make_shared<Connection>(state_));
    }
    for (const auto& c : state_->connections) {
        c->connect();
    }
}

Pool::~Pool() {
    // callbacks still waiting never run once the pool is gone
    state_->closed = true;
    state_->queue.clear();
    for (const auto& c : state_->connections) {
        c->shutdown();
    }
    state_->connections.clear();
}

void Pool::query(std::string sql, Params params, Done done) {
    auto& s = *state_;
    if (s.queue.size() >= s.settings.max_queued) {
        ++s.failed;
        asio::post(s.io, [done = std::move(done)] { deliver(done, Result::failure("too many queries waiting")); });
        return;
    }
    s.queue.push_back({.sql = std::move(sql), .params = std::move(params), .done = std::move(done)});
    asio::post(s.io, [state = state_] {
        if (state->closed) {
            return;
        }
        if (!state->any_up()) {
            state->fail_queued(state->last_error.empty() ? std::string{"no connection"} : state->last_error);
            return;
        }
        state->dispatch();
    });
}

Stats Pool::stats() const noexcept {
    Stats out{.queued = state_->queue.size(), .completed = state_->completed, .failed = state_->failed};
    for (const auto& c : state_->connections) {
        const auto phase = c->phase();
        out.connected += phase == Connection::Phase::Ready || phase == Connection::Phase::Busy ? 1U : 0U;
        out.busy += phase == Connection::Phase::Busy ? 1U : 0U;
    }
    return out;
}

} // namespace raikiri::pg
