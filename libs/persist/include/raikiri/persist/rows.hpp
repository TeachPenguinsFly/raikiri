#pragma once

// Row model for the cold plane. The journal→row mapping is pure code
// (mapper.hpp) so it unit-tests without a database; PgSink (sink.hpp) is the
// only thing that talks SQL. Market data (BBO/trades) deliberately never
// reaches Postgres — the journal is its store; ClickHouse/Timescale is the
// documented path if MD analytics are ever wanted.

#include <cstdint>
#include <string>
#include <vector>

namespace raikiri::persist {

struct OrderEventRow {
    std::uint64_t journal_seq{0};
    std::uint64_t client_order_id{0};
    std::string event_type;
    std::uint64_t instrument_id{0};
    std::int64_t price_ticks{0};
    std::int64_t qty_lots{0};
    std::int64_t leaves_lots{0};
    std::int64_t fee_e9{0};
    std::string exec_id;
    std::uint64_t ts_event{0};
    std::uint64_t ts_recorded{0};
};

// Current-state projection of one order; the event log is the truth.
struct OrderUpsert {
    std::uint64_t client_order_id{0};
    std::string exchange_client_order_id;
    std::string exchange_order_id;
    std::uint64_t instrument_id{0};
    std::string side;
    std::string order_type;
    std::string tif;
    std::int16_t flags{0};
    std::int64_t price_ticks{0};
    std::int64_t qty_lots{0};
    std::int64_t leaves_lots{0};
    std::string status; // empty = keep existing status
    std::uint64_t ts_event{0};
};

struct ExecutionRow {
    std::string venue;
    std::string exec_id;
    std::string exchange_order_id;
    std::uint64_t client_order_id{0};
    std::uint64_t instrument_id{0};
    std::string side;
    std::int64_t price_ticks{0};
    std::int64_t qty_lots{0};
    std::int64_t fee_e9{0};
    std::string liquidity;
    std::uint64_t ts_event{0};
};

// One increment of realized money, idempotent on source_id. This ledger is the
// truth for PnL; the positions projection is rebuildable from it.
//
// venue and symbol are the durable identity. instrument_id rides along for
// correlation with a running process, but its symbol half is a catalogue
// load-order index and points at a different instrument once a delisting
// shifts the list.
struct PnlIncrementRow {
    std::string source_id; // "<kind>:<venue>:<exec_id>"
    std::string kind;      // realized | fee | funding
    std::string settle_side; // base | quote
    std::uint16_t strategy_id{0};
    std::string venue;
    std::string exchange_symbol;
    std::uint64_t instrument_id{0};
    std::uint64_t client_order_id{0};
    std::int64_t amount_e9{0};
    std::uint64_t ts_event{0};
};

struct FundingRow {
    std::uint64_t instrument_id{0};
    std::uint64_t ts_event{0};
    std::int64_t rate_e9{0};
    std::int64_t predicted_e9{0};
    std::int64_t mark_ticks{0};
    std::uint64_t next_funding_ts{0};
    std::uint32_t interval_s{0};
};

struct ConnectivityRow {
    std::uint64_t journal_seq{0};
    std::string venue;
    std::string event;
    std::uint32_t stream_id{0};
    std::uint64_t ts_recorded{0};
};

struct LatencyRow {
    std::uint64_t journal_seq{0};
    std::string venue;
    std::uint32_t metric{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};
    std::uint64_t count{0};
    std::uint64_t ts_recorded{0};
};

struct RowBatch {
    std::vector<OrderEventRow> order_events;
    std::vector<OrderUpsert> order_upserts;
    std::vector<ExecutionRow> executions;
    std::vector<PnlIncrementRow> pnl_increments;
    std::vector<FundingRow> funding;
    std::vector<ConnectivityRow> connectivity;
    std::vector<LatencyRow> latency;
    std::uint64_t last_seq{0}; // highest journal seq contributing to this batch

    [[nodiscard]] std::size_t size() const noexcept {
        return order_events.size() + order_upserts.size() + executions.size() +
               pnl_increments.size() + funding.size() + connectivity.size() + latency.size();
    }

    void clear() noexcept {
        order_events.clear();
        order_upserts.clear();
        executions.clear();
        pnl_increments.clear();
        funding.clear();
        connectivity.clear();
        latency.clear();
    }
};

} // namespace raikiri::persist
