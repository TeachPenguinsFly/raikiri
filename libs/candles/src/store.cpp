#include <raikiri/candles/store.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <utility>
#include <vector>

namespace raikiri::candles {

namespace {

// rows a statement carries; a minute of every busy market is a few thousand
constexpr std::size_t k_batch = 500;

// a venue minute is whole, so it replaces what is there: our own minute at either end of a gap is partial. Our
// minutes fold into each other (a restart splits one in two) and never overwrite the venue's
constexpr std::string_view k_upsert = R"(
    ON CONFLICT (instrument_id, minute) DO UPDATE SET
        open   = CASE WHEN EXCLUDED.source = 1 THEN EXCLUDED.open ELSE c.open END,
        high   = CASE WHEN EXCLUDED.source = 1 THEN EXCLUDED.high WHEN c.source = 1 THEN c.high
                      ELSE GREATEST(c.high, EXCLUDED.high) END,
        low    = CASE WHEN EXCLUDED.source = 1 THEN EXCLUDED.low WHEN c.source = 1 THEN c.low
                      ELSE LEAST(c.low, EXCLUDED.low) END,
        close  = CASE WHEN EXCLUDED.source = 1 THEN EXCLUDED.close WHEN c.source = 1 THEN c.close
                      ELSE EXCLUDED.close END,
        volume = CASE WHEN EXCLUDED.source = 1 THEN EXCLUDED.volume WHEN c.source = 1 THEN c.volume
                      ELSE c.volume + EXCLUDED.volume END,
        trades = CASE WHEN EXCLUDED.source = 1 THEN EXCLUDED.trades WHEN c.source = 1 THEN c.trades
                      ELSE c.trades + EXCLUDED.trades END,
        source = GREATEST(c.source, EXCLUDED.source)
)";

// one row per instrument and minute, folded the way the upsert folds rows already stored: postgres refuses a statement
// that would touch the same row twice
// a minute of every attached instrument goes in one statement
constexpr std::size_t k_metric_batch = 8192;

std::string sql_value(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "NULL";
}

std::vector<Candle> folded(std::span<const Candle> candles) {
    std::vector<Candle> rows{candles.begin(), candles.end()};
    std::ranges::stable_sort(rows, [](const Candle& a, const Candle& b) {
        return a.instrument.packed != b.instrument.packed ? a.instrument.packed < b.instrument.packed
                                                          : a.minute < b.minute;
    });
    std::vector<Candle> out;
    out.reserve(rows.size());
    for (const auto& c : rows) {
        if (out.empty() || out.back().instrument.packed != c.instrument.packed || out.back().minute != c.minute) {
            out.push_back(c);
            continue;
        }
        auto& kept = out.back();
        if (c.source == Source::Venue) {
            kept = c;
        } else if (kept.source == Source::Live) {
            kept.high = std::max(kept.high, c.high);
            kept.low = std::min(kept.low, c.low);
            kept.close = c.close;
            kept.volume += c.volume;
            kept.trades += c.trades;
        }
    }
    return out;
}

} // namespace

struct Store::Impl {
    pqxx::connection conn;

    explicit Impl(const std::string& dsn) : conn{dsn} {}
};

Store::Store(const std::string& dsn) : impl_{std::make_unique<Impl>(dsn)} {}

Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
Store::~Store() = default;

void Store::write(std::span<const Candle> unfolded) {
    const auto rows = folded(unfolded);
    const std::span<const Candle> candles{rows};
    pqxx::work tx{impl_->conn};
    for (std::size_t start = 0; start < candles.size(); start += k_batch) {
        const auto chunk = candles.subspan(start, std::min(k_batch, candles.size() - start));
        std::string sql = "INSERT INTO raikiri.candles_1m AS c (instrument_id, minute, open, high, low, close, volume, "
                          "trades, source) "
                          "VALUES ";
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto& c = chunk[i];
            sql += std::format("{}({},{},{},{},{},{},{},{},{})",
                               i == 0 ? "" : ",",
                               static_cast<std::int64_t>(c.instrument.packed),
                               c.minute,
                               c.open,
                               c.high,
                               c.low,
                               c.close,
                               c.volume,
                               c.trades,
                               static_cast<int>(c.source));
        }
        sql += k_upsert;
        tx.exec(sql);
    }
    tx.commit();
}

std::unordered_map<std::uint64_t, Store::Span> Store::spans(std::int32_t from) {
    pqxx::read_transaction tx{impl_->conn};
    std::unordered_map<std::uint64_t, Span> out;
    for (const auto& [id, first, last] : tx.query<std::int64_t, std::int32_t, std::int32_t>(
             "SELECT instrument_id, min(minute), max(minute) FROM raikiri.candles_1m WHERE minute >= $1 GROUP BY 1",
             pqxx::params{from})) {
        out[static_cast<std::uint64_t>(id)] = {.first = first, .last = last};
    }
    return out;
}

std::vector<Candle> Store::read(InstrumentId instrument, std::int32_t from, std::int32_t to) {
    pqxx::read_transaction tx{impl_->conn};
    std::vector<Candle> out;
    for (const auto& [minute, open, high, low, close, volume, trades, source] :
         tx.query<std::int32_t,
                  std::int64_t,
                  std::int64_t,
                  std::int64_t,
                  std::int64_t,
                  std::int64_t,
                  std::int32_t,
                  std::int16_t>("SELECT minute, open, high, low, close, volume, trades, source FROM raikiri.candles_1m "
                                "WHERE instrument_id = $1 AND minute >= $2 AND minute < $3 ORDER BY minute",
                                pqxx::params{static_cast<std::int64_t>(instrument.packed), from, to})) {
        out.push_back({.instrument = instrument,
                       .minute = minute,
                       .open = open,
                       .high = high,
                       .low = low,
                       .close = close,
                       .volume = volume,
                       .trades = trades,
                       .source = static_cast<Source>(source)});
    }
    return out;
}

std::uint64_t Store::prune(std::int32_t before) {
    pqxx::work tx{impl_->conn};
    const auto result = tx.exec("DELETE FROM raikiri.candles_1m WHERE minute < $1", pqxx::params{before});
    tx.commit();
    return static_cast<std::uint64_t>(result.affected_rows());
}

void Store::write_metrics(std::span<const MetricSample> samples) {
    pqxx::work tx{impl_->conn};
    for (std::size_t start = 0; start < samples.size(); start += k_metric_batch) {
        const auto chunk = samples.subspan(start, std::min(k_metric_batch, samples.size() - start));
        std::string sql = "INSERT INTO raikiri.metrics_1m AS m (instrument_id, minute, funding_rate, mark_price, "
                          "index_price, open_interest) VALUES ";
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto& s = chunk[i];
            sql += std::format("{}({},{},{},{},{},{})",
                               i == 0 ? "" : ",",
                               static_cast<std::int64_t>(s.instrument.packed),
                               s.minute,
                               sql_value(s.funding_rate),
                               sql_value(s.mark),
                               sql_value(s.index),
                               sql_value(s.open_interest));
        }
        sql += " ON CONFLICT (instrument_id, minute) DO UPDATE SET"
               " funding_rate = COALESCE(EXCLUDED.funding_rate, m.funding_rate),"
               " mark_price = COALESCE(EXCLUDED.mark_price, m.mark_price),"
               " index_price = COALESCE(EXCLUDED.index_price, m.index_price),"
               " open_interest = COALESCE(EXCLUDED.open_interest, m.open_interest)";
        tx.exec(sql);
    }
    tx.commit();
}

void Store::write_funding(std::span<const MetricSample> samples) {
    // a statement takes a row once, and two pages of one answer may share a funding time
    std::vector<MetricSample> rows;
    rows.reserve(samples.size());
    for (const auto& s : samples) {
        if (s.funding_rate.has_value()) {
            rows.push_back(s);
        }
    }
    std::ranges::sort(rows, [](const MetricSample& a, const MetricSample& b) {
        return std::pair{a.instrument.packed, a.minute} < std::pair{b.instrument.packed, b.minute};
    });
    const auto repeats = std::ranges::unique(rows, [](const MetricSample& a, const MetricSample& b) {
        return a.instrument == b.instrument && a.minute == b.minute;
    });
    rows.erase(repeats.begin(), repeats.end());
    pqxx::work tx{impl_->conn};
    for (std::size_t start = 0; start < rows.size(); start += k_metric_batch) {
        std::string sql = "INSERT INTO raikiri.metrics_1m AS m (instrument_id, minute, funding_rate) VALUES ";
        const auto end = std::min(rows.size(), start + k_metric_batch);
        for (std::size_t i = start; i < end; ++i) {
            sql += std::format("{}({},{},{})",
                               i == start ? "" : ",",
                               static_cast<std::int64_t>(rows[i].instrument.packed),
                               rows[i].minute,
                               *rows[i].funding_rate);
        }
        sql += " ON CONFLICT (instrument_id, minute) DO UPDATE SET"
               " funding_rate = COALESCE(m.funding_rate, EXCLUDED.funding_rate)";
        tx.exec(sql);
    }
    tx.commit();
}

std::unordered_set<std::uint64_t> Store::funded(std::span<const std::uint64_t> instruments, std::int32_t before) {
    std::unordered_set<std::uint64_t> out;
    if (instruments.empty()) {
        return out;
    }
    // each instrument is one probe of the primary key, where a DISTINCT over the table would read all of it
    std::string sql = "SELECT i FROM unnest(ARRAY[";
    for (std::size_t i = 0; i < instruments.size(); ++i) {
        sql += std::format("{}{}", i == 0 ? "" : ",", static_cast<std::int64_t>(instruments[i]));
    }
    sql += "]::bigint[]) AS i WHERE EXISTS (SELECT 1 FROM raikiri.metrics_1m m WHERE m.instrument_id = i"
           " AND m.minute < $1 AND m.funding_rate IS NOT NULL)";
    pqxx::read_transaction tx{impl_->conn};
    for (const auto& [id] : tx.query<std::int64_t>(sql, pqxx::params{before})) {
        out.insert(static_cast<std::uint64_t>(id));
    }
    return out;
}

std::uint64_t Store::prune_metrics(std::int32_t before) {
    pqxx::work tx{impl_->conn};
    const auto result = tx.exec("DELETE FROM raikiri.metrics_1m WHERE minute < $1", pqxx::params{before});
    tx.commit();
    return static_cast<std::uint64_t>(result.affected_rows());
}

} // namespace raikiri::candles
