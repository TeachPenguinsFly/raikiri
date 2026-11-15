#pragma once

#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/types.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/http/server.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::admin {

// work admind does on its own loop, on a schedule, so a request reads an answer kept warm instead of computing it
class Jobs {
public:
    struct Status {
        std::string name;
        std::uint64_t period_ms{0};
        std::uint64_t runs{0};
        double last_ms{0.0};     // how long the last run took
        std::int64_t age_ms{-1}; // since the last run; -1 before the first
    };

    void add(std::string name, std::chrono::milliseconds period, std::function<void()> run);
    // runs every job that is due, and returns when the next one will be
    MonoNanos tick(MonoNanos now);
    // runs one now whatever its schedule, e.g. when the gateways it reads have changed
    void run_now(std::string_view name, MonoNanos now);
    [[nodiscard]] std::vector<Status> status(MonoNanos now) const;
    // GET /cache/jobs; empty for any other path so a composed router falls through
    [[nodiscard]] std::optional<http::Response> route(const http::Request& req, MonoNanos now) const;

private:
    struct Job {
        std::string name;
        std::uint64_t period_ns{0};
        std::function<void()> run;
        std::uint64_t due_ns{0};
        std::uint64_t last_ns{0};
        std::uint64_t runs{0};
        double last_ms{0.0};
    };

    static void run(Job& job, MonoNanos now);
    std::vector<Job> jobs_;
};

// moves each listing priced more than 20% from its market (the median of three prices or more, else the busiest one)
// into a market of its own keyed `key@venue`, so a ticker another venue gives an unrelated coin is never paired
void split_strangers(std::vector<Market>& markets);

// the market list ranked by turnover, rebuilt by a job rather than on every request
class MarketsCache {
public:
    explicit MarketsCache(const Registry& registry) : registry_{&registry} {}

    void refresh();
    // the warm list, filtered; straight from the registry until the first refresh
    [[nodiscard]] std::vector<Market> query(std::string_view text, std::size_t limit) const;

private:
    const Registry* registry_;
    std::vector<Market> all_;
    bool warm_{false};
};

} // namespace raikiri::admin
