#include <raikiri/persist/sink.hpp>

#include <algorithm>
#include <fstream>
#include <pqxx/pqxx>
#include <sstream>
#include <stdexcept>

namespace raikiri::persist {

struct PgSink::Impl {
    pqxx::connection conn;

    explicit Impl(const std::string& dsn) : conn{dsn} {}
};

PgSink::PgSink(const std::string& dsn) : impl_{std::make_unique<Impl>(dsn)} {}

PgSink::PgSink(PgSink&&) noexcept = default;

PgSink::~PgSink() = default;

void PgSink::exec_file(const std::string& path) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error{"cannot read sql file: " + path};
    }
    std::stringstream buf;
    buf << file.rdbuf();
    pqxx::work tx{impl_->conn};
    tx.exec(buf.str());
    tx.commit();
}

std::uint64_t PgSink::load_cursor(const std::string& journal) {
    pqxx::work tx{impl_->conn};
    const auto row = tx.exec("SELECT last_seq FROM raikiri.persist_cursors WHERE journal = $1",
                             pqxx::params{journal});
    tx.commit();
    if (row.empty()) {
        return 0;
    }
    return row[0][0].as<std::uint64_t>();
}

std::int64_t PgSink::count_rows(const std::string& table) {
    if (table.empty() || !std::ranges::all_of(table, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || c == '_';
        })) {
        throw std::invalid_argument{"count_rows: not a bare table name"};
    }
    pqxx::work tx{impl_->conn};
    const auto row = tx.exec("SELECT count(*) FROM raikiri." + table);
    tx.commit();
    return row[0][0].as<std::int64_t>();
}

void PgSink::commit(const std::string& journal, const RowBatch& batch) {
    pqxx::work tx{impl_->conn};

    for (const auto& r : batch.order_events) {
        // null on non-rejections and on pre-version-2 records
        tx.exec("INSERT INTO raikiri.order_events (journal, journal_seq, client_order_id,"
                " event_type, instrument_id, price_ticks, qty_lots, leaves_lots, fee_e9,"
                " exec_id, ts_event, ts_recorded, reject_class, reject_code)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,NULLIF($10,''),$11,$12,"
                "         NULLIF($13,''), CASE WHEN $14 THEN $15::integer ELSE NULL::integer END)"
                " ON CONFLICT (journal, journal_seq) DO NOTHING",
                pqxx::params{journal,
                             r.journal_seq,
                             r.client_order_id,
                             r.event_type,
                             r.instrument_id,
                             r.price_ticks,
                             r.qty_lots,
                             r.leaves_lots,
                             r.fee_e9,
                             r.exec_id,
                             r.ts_event,
                             r.ts_recorded,
                             r.reject_class,
                             r.has_reason,
                             r.reject_code});
    }

    for (const auto& r : batch.order_upserts) {
        tx.exec("INSERT INTO raikiri.orders (client_order_id, exchange_client_order_id,"
                " exchange_order_id, instrument_id, side, order_type, tif, flags, price_ticks,"
                " qty_lots, leaves_lots, status, ts_created, ts_updated,"
                " reject_class, reject_code)"
                " VALUES ($1,NULLIF($2,''),NULLIF($3,''),$4,$5,$6,$7,$8,$9,$10,$11,"
                "         COALESCE(NULLIF($12,''),'initialized'),$13,$13,"
                "         NULLIF($14,''), CASE WHEN $15 THEN $16::integer ELSE NULL::integer END)"
                " ON CONFLICT (client_order_id) DO UPDATE SET"
                "  exchange_client_order_id ="
                "    COALESCE(raikiri.orders.exchange_client_order_id, "
                "EXCLUDED.exchange_client_order_id),"
                "  exchange_order_id = COALESCE(NULLIF(EXCLUDED.exchange_order_id,''),"
                "                               raikiri.orders.exchange_order_id),"
                "  leaves_lots = EXCLUDED.leaves_lots,"
                "  status = CASE WHEN EXCLUDED.status IS NULL OR EXCLUDED.status = ''"
                "                THEN raikiri.orders.status ELSE EXCLUDED.status END,"
                "  ts_updated = GREATEST(raikiri.orders.ts_updated, EXCLUDED.ts_updated),"
                // keep the reason once written; later events carry none
                "  reject_class = COALESCE(EXCLUDED.reject_class, raikiri.orders.reject_class),"
                "  reject_code  = COALESCE(EXCLUDED.reject_code,  raikiri.orders.reject_code)"
                " WHERE EXCLUDED.ts_updated >= raikiri.orders.ts_updated",
                pqxx::params{r.client_order_id,
                             r.exchange_client_order_id,
                             r.exchange_order_id,
                             r.instrument_id,
                             r.side,
                             r.order_type,
                             r.tif,
                             r.flags,
                             r.price_ticks,
                             r.qty_lots,
                             r.leaves_lots,
                             r.status,
                             r.ts_event,
                             r.reject_class,
                             r.has_reason,
                             r.reject_code});
    }

    for (const auto& r : batch.executions) {
        tx.exec("INSERT INTO raikiri.executions (venue, exec_id, exchange_order_id,"
                " client_order_id, instrument_id, side, price_ticks, qty_lots, fee_e9,"
                " liquidity, ts_event)"
                " VALUES ($1,$2,NULLIF($3,''),$4,$5,$6,$7,$8,$9,$10,$11)"
                " ON CONFLICT (venue, exec_id) DO NOTHING",
                pqxx::params{r.venue,
                             r.exec_id,
                             r.exchange_order_id,
                             r.client_order_id,
                             r.instrument_id,
                             r.side,
                             r.price_ticks,
                             r.qty_lots,
                             r.fee_e9,
                             r.liquidity,
                             r.ts_event});
    }

    for (const auto& r : batch.pnl_increments) {
        // do nothing on conflict so a replay is a no-op
        tx.exec("INSERT INTO raikiri.pnl_increments (source_id, account_id, kind, amount_e9,"
                " ts_event, strategy_id, venue, exchange_symbol, instrument_id, client_order_id,"
                " settle_side)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)"
                " ON CONFLICT (source_id) DO NOTHING",
                pqxx::params{r.source_id,
                             r.venue,
                             r.kind,
                             r.amount_e9,
                             r.ts_event,
                             static_cast<std::int16_t>(r.strategy_id),
                             r.venue,
                             r.exchange_symbol,
                             r.instrument_id,
                             r.client_order_id,
                             r.settle_side});
    }

    for (const auto& r : batch.recon_breaks) {
        tx.exec("INSERT INTO raikiri.recon_breaks (venue, exchange_symbol, ts_seen,"
                " local_qty_lots, venue_qty_lots, detail)"
                " VALUES ($1,$2,$3,$4,$5,$6)"
                " ON CONFLICT (venue, exchange_symbol, ts_seen) DO NOTHING",
                pqxx::params{r.venue,
                             r.exchange_symbol,
                             r.ts_seen,
                             r.local_qty_lots,
                             r.venue_qty_lots,
                             r.detail});
    }

    // upsert; the ts_updated guard keeps the newer row under out-of-order replay
    for (const auto& r : batch.positions) {
        tx.exec("INSERT INTO raikiri.positions (strategy_id, venue, exchange_symbol,"
                " instrument_id, net_qty_lots, cost_e9, realized_e9, fees_e9, settle_side,"
                " fill_count, ts_opened, ts_updated)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)"
                " ON CONFLICT (strategy_id, venue, exchange_symbol) DO UPDATE SET"
                " instrument_id = EXCLUDED.instrument_id,"
                " net_qty_lots = EXCLUDED.net_qty_lots,"
                " cost_e9 = EXCLUDED.cost_e9,"
                " realized_e9 = EXCLUDED.realized_e9,"
                " fees_e9 = EXCLUDED.fees_e9,"
                " settle_side = EXCLUDED.settle_side,"
                " fill_count = EXCLUDED.fill_count,"
                " ts_updated = EXCLUDED.ts_updated"
                " WHERE EXCLUDED.ts_updated >= raikiri.positions.ts_updated",
                pqxx::params{r.strategy_id,
                             r.venue,
                             r.exchange_symbol,
                             r.instrument_id,
                             r.net_qty_lots,
                             r.cost_e9,
                             r.realized_e9,
                             r.fees_e9,
                             r.settle_side,
                             r.fill_count,
                             r.ts_opened,
                             r.ts_updated});
    }

    for (const auto& r : batch.funding) {
        tx.exec("INSERT INTO raikiri.funding_observations (instrument_id, ts_event, rate_e9,"
                " predicted_e9, mark_ticks, next_funding_ts, interval_s, accrual)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8)"
                " ON CONFLICT (instrument_id, ts_event) DO NOTHING",
                pqxx::params{r.instrument_id,
                             r.ts_event,
                             r.rate_e9,
                             r.predicted_e9,
                             r.mark_ticks,
                             r.next_funding_ts,
                             r.interval_s,
                             r.accrual});
    }

    // a generation closes other windows holding this id at its start, so replay order does not
    // matter
    for (const auto& r : batch.instruments) {
        tx.exec("UPDATE raikiri.instruments SET valid_to = $3"
                " WHERE instrument_id = $1 AND generation <> $2 AND valid_from < $3"
                " AND (valid_to IS NULL OR valid_to > $3)",
                pqxx::params{r.instrument_id, r.generation, r.valid_from});
        tx.exec("INSERT INTO raikiri.instruments (instrument_id, generation, venue, market_type,"
                " local_symbol, exchange_symbol, tick_size_e9, lot_size_e9, min_notional_e9,"
                " contract_value_e9, contract_kind, valid_from, asset_class, underlying, valid_to)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,"
                "  (SELECT MIN(valid_from) FROM raikiri.instruments"
                "    WHERE instrument_id = $1 AND generation <> $2 AND valid_from > $12))"
                " ON CONFLICT (instrument_id, generation) DO NOTHING",
                pqxx::params{r.instrument_id,
                             r.generation,
                             r.venue,
                             r.market_type,
                             r.local_symbol,
                             r.exchange_symbol,
                             r.tick_size_e9,
                             r.lot_size_e9,
                             r.min_notional_e9,
                             r.contract_value_e9,
                             r.contract_kind,
                             r.valid_from,
                             r.asset_class,
                             r.underlying});
    }

    for (const auto& r : batch.connectivity) {
        tx.exec("INSERT INTO raikiri.connectivity_events (journal, journal_seq, venue, event,"
                " stream_id, ts_recorded) VALUES ($1,$2,$3,$4,$5,$6)"
                " ON CONFLICT (journal, journal_seq) DO NOTHING",
                pqxx::params{journal, r.journal_seq, r.venue, r.event, r.stream_id, r.ts_recorded});
    }

    for (const auto& r : batch.latency) {
        tx.exec("INSERT INTO raikiri.latency_samples (journal, journal_seq, venue, metric,"
                " p50_ns, p99_ns, p999_ns, max_ns, sample_count, ts_recorded)"
                " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)"
                " ON CONFLICT (journal, journal_seq) DO NOTHING",
                pqxx::params{journal,
                             r.journal_seq,
                             r.venue,
                             r.metric,
                             r.p50_ns,
                             r.p99_ns,
                             r.p999_ns,
                             r.max_ns,
                             r.count,
                             r.ts_recorded});
    }

    tx.exec("INSERT INTO raikiri.persist_cursors (journal, last_seq) VALUES ($1,$2)"
            " ON CONFLICT (journal) DO UPDATE SET last_seq = EXCLUDED.last_seq,"
            " updated_at = now()",
            pqxx::params{journal, batch.last_seq});
    tx.commit();
}

} // namespace raikiri::persist
