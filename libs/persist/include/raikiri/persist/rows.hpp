#pragma once

// market data never reaches postgres; the journal is its store

#include <cstdint>
#include <optional>
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
    // empty on other kinds and on records before schema version 2
    std::string reject_class;
    std::int32_t reject_code{0};
    bool has_reason{false};
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
    std::string reject_class;
    std::int32_t reject_code{0};
    bool has_reason{false};
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

// idempotent on source_id; venue and symbol are the durable identity, instrument_id only a
// load-order index
struct PnlIncrementRow {
    std::string source_id;   // "<kind>:<venue>:<exec_id>"
    std::string kind;        // realized | fee | funding
    std::string settle_side; // base | quote
    std::uint16_t strategy_id{0};
    std::string venue;
    std::string exchange_symbol;
    std::uint64_t instrument_id{0};
    std::uint64_t client_order_id{0};
    std::int64_t amount_e9{0};
    std::uint64_t ts_event{0};
};

// never overwritten
struct ReconBreakRow {
    std::string venue;
    std::string exchange_symbol;
    std::uint64_t ts_seen{0};
    std::int64_t local_qty_lots{0};
    std::int64_t venue_qty_lots{0};
    std::string detail;
};

// upserted on (strategy, venue, symbol)
struct PositionRow {
    std::uint16_t strategy_id{0};
    std::string venue;
    std::string exchange_symbol;
    std::uint64_t instrument_id{0};
    std::int64_t net_qty_lots{0};
    std::int64_t cost_e9{0};
    std::int64_t realized_e9{0};
    std::int64_t fees_e9{0};
    std::string settle_side;
    std::uint32_t fill_count{0};
    std::uint64_t ts_opened{0};
    std::uint64_t ts_updated{0};
};

struct FundingRow {
    std::uint64_t instrument_id{0};
    std::uint64_t ts_event{0};
    std::int64_t rate_e9{0};
    std::int64_t predicted_e9{0};
    std::int64_t mark_ticks{0};
    std::uint64_t next_funding_ts{0};
    std::uint32_t interval_s{0};
    std::optional<std::string> accrual; // null: unknown, or journalled before version 3
};

// One catalogue entry for one gateway generation, valid from valid_from until
// the same id's next generation starts.
struct InstrumentRow {
    std::uint64_t instrument_id{0};
    std::uint64_t generation{0};
    std::string venue;
    std::string market_type;
    std::string exchange_symbol;
    std::string local_symbol;
    std::string contract_kind;
    std::string asset_class{"unknown"};
    std::string underlying;
    std::int64_t tick_size_e9{0};
    std::int64_t lot_size_e9{0};
    std::int64_t min_notional_e9{0};
    std::int64_t contract_value_e9{0};
    std::uint64_t valid_from{0};
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
    std::vector<ReconBreakRow> recon_breaks;
    std::vector<PositionRow> positions;
    std::vector<FundingRow> funding;
    std::vector<InstrumentRow> instruments;
    std::vector<ConnectivityRow> connectivity;
    std::vector<LatencyRow> latency;
    std::uint64_t last_seq{0}; // highest journal seq contributing to this batch

    [[nodiscard]] std::size_t size() const noexcept {
        return order_events.size() + order_upserts.size() + executions.size() +
               pnl_increments.size() + recon_breaks.size() + positions.size() + funding.size() +
               instruments.size() + connectivity.size() + latency.size();
    }

    void clear() noexcept {
        order_events.clear();
        order_upserts.clear();
        executions.clear();
        pnl_increments.clear();
        recon_breaks.clear();
        positions.clear();
        funding.clear();
        instruments.clear();
        connectivity.clear();
        latency.clear();
    }
};

} // namespace raikiri::persist
