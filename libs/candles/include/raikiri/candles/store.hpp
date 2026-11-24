#pragma once

#include <raikiri/candles/candle.hpp>
#include <raikiri/candles/metrics.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::candles {

// raikiri.candles_1m and raikiri.metrics_1m in postgres; one connection, used from one thread
class Store {
public:
    explicit Store(const std::string& dsn);
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    ~Store();

    // our minutes fold into one already stored (a restart splits a minute in two) but never overwrite the venue's; a
    // venue minute is whole and replaces whatever is there
    void write(std::span<const Candle> candles);

    struct Span {
        std::int32_t first{0};
        std::int32_t last{0};
    };

    // the first and last stored minute at or after `from` of every instrument that has one
    [[nodiscard]] std::unordered_map<std::uint64_t, Span> spans(std::int32_t from);
    // minutes in [from, to), oldest first
    [[nodiscard]] std::vector<Candle> read(InstrumentId instrument, std::int32_t from, std::int32_t to);
    // drops minutes older than `before`; returns how many
    std::uint64_t prune(std::int32_t before);

    // at most one sample per instrument and minute; a minute sampled twice (a restart inside it) keeps the newer value
    // of each field and the older where the newer has none
    void write_metrics(std::span<const MetricSample> samples);
    // a venue's past funding: fills the rate where a minute has none and leaves a sampled one alone
    void write_funding(std::span<const MetricSample> samples);
    // those of `instruments` with a funding rate stored before `before`
    [[nodiscard]] std::unordered_set<std::uint64_t> funded(std::span<const std::uint64_t> instruments,
                                                           std::int32_t before);
    std::uint64_t prune_metrics(std::int32_t before);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::candles
