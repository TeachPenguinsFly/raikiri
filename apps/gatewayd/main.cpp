// gatewayd: one venue and one market per process; owns the venue's shm segment and runs the hot loop

#include <raikiri/common/platform.hpp>
#include <raikiri/config/one_password.hpp>
#include <raikiri/config/rest_limits.hpp>
#include <raikiri/config/secret.hpp>
#include <raikiri/config/settings.hpp>
#include <raikiri/domain/asset_class.hpp>
#include <raikiri/domain/demo_instruments.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/introspect/clock_probe.hpp>
#include <raikiri/introspect/files.hpp>
#include <raikiri/introspect/server_time.hpp>
#include <raikiri/journal/internal.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/schema/codec.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/venues/apex/apex_venue.hpp>
#include <raikiri/venues/aster/aster_venue.hpp>
#include <raikiri/venues/backpack/backpack_venue.hpp>
#include <raikiri/venues/binance/binance_venue.hpp>
#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/bitunix/bitunix_venue.hpp>
#include <raikiri/venues/blofin/blofin_venue.hpp>
#include <raikiri/venues/bybit/bybit_venue.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/cryptocom/cryptocom_venue.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/dydx/dydx_venue.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>
#include <raikiri/venues/extended/extended_venue.hpp>
#include <raikiri/venues/gate/gate_venue.hpp>
#include <raikiri/venues/grvt/grvt_venue.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/lbank/lbank_venue.hpp>
#include <raikiri/venues/lighter/lighter_venue.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/okx/okx_venue.hpp>
#include <raikiri/venues/pacifica/pacifica_venue.hpp>
#include <raikiri/venues/phemex/phemex_venue.hpp>
#include <raikiri/venues/sim/sim_venue.hpp>
#include <raikiri/venues/toobit/toobit_venue.hpp>
#include <raikiri/venues/weex/weex_venue.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <fmt/format.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "latency_meter.hpp"
#include "private_pulls.hpp"
#include "stats_collector.hpp"

namespace {

struct SubKinds {
    bool trades{true};
    bool depth{true};
    bool funding{true};
};

// bbo is required; an unknown channel name is refused
[[nodiscard]] std::expected<SubKinds, std::string> parse_channels(std::string_view spec) {
    SubKinds out{.trades = false, .depth = false, .funding = false};
    bool named_bbo = false;
    std::size_t at = 0;
    while (at <= spec.size()) {
        const auto comma = spec.find(',', at);
        const auto name = spec.substr(at, comma == std::string_view::npos ? comma : comma - at);
        if (name == "bbo") {
            named_bbo = true;
        } else if (name == "trades") {
            out.trades = true;
        } else if (name == "depth") {
            out.depth = true;
        } else if (name == "funding") {
            out.funding = true;
        } else if (!name.empty()) {
            return std::unexpected{fmt::format("unknown channel '{}' — bbo, trades, depth or funding", name)};
        }
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    if (!named_bbo) {
        return std::unexpected{std::string{"the quote cannot be dropped — name bbo alongside whatever else you want"}};
    }
    return out;
}

[[nodiscard]] constexpr std::string_view error_class_name(raikiri::exec::ErrorClass cls) noexcept {
    using enum raikiri::exec::ErrorClass;
    switch (cls) {
    case Terminal:
        return "terminal";
    case Retryable:
        return "retryable";
    case RateLimited:
        return "rate-limited";
    case AuthFailure:
        return "auth";
    case Connectivity:
        return "connectivity";
    }
    return "unknown";
}

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

using msgs = raikiri_journal::schema::messages;

// adapter events to shm and journal; single-threaded
class ShmSink final : public raikiri::exec::GatewaySink {
public:
    ShmSink(raikiri::VenueId venue,
            raikiri::shm::GatewayChannels channels,
            raikiri::journal::JournalWriter& journal,
            raikiri::gatewayd::LatencyMeter& latency)
        : venue_{venue}, ch_{channels}, journal_{&journal}, latency_{&latency} {}

    void on_bbo(const raikiri::Bbo& bbo) noexcept override {
        ++counts_.bbo;
        const auto idx = bbo.instrument.symbol().raw;
        if (idx < raikiri::shm::k_bbo_cells) {
            ch_.bbo->write(idx, bbo);
            priced_ns_[idx] = std::max<std::uint64_t>(bbo.ts_init.ns, 1);
        }
        latency_->published(raikiri::gatewayd::Channel::Bbo, bbo.ts_event, bbo.ts_init);
    }

    void on_trade(const raikiri::TradeTick& trade) noexcept override {
        ++counts_.trades;
        raikiri::shm::MdEvent event{};
        event.kind = raikiri::shm::MdKind::Trade;
        event.aggressor = trade.aggressor;
        event.instrument = trade.instrument;
        event.price = trade.price;
        event.qty = trade.qty;
        event.ts_event = trade.ts_event;
        event.ts_init = trade.ts_init;
        event.venue_seq = ++md_seq_;
        ch_.md_events->publish(event);
        latency_->published(raikiri::gatewayd::Channel::Trades, trade.ts_event, trade.ts_init);
    }

    // dropping a depth publish is safe because the cell holds the latest book; do not copy this to the trade ring
    // asks only; on_depth closes the window, so asking twice is safe
    [[nodiscard]] bool wants_depth(raikiri::InstrumentId instrument, raikiri::UnixNanos now) noexcept override {
        const auto idx = instrument.symbol().raw;
        if (idx >= raikiri::shm::k_depth_cells) {
            return false;
        }
        if (now.ns - depth_published_ns_[idx] < k_depth_window_ns) {
            ++counts_.depth_coalesced;
            return false;
        }
        return true;
    }

    void on_depth(const raikiri::BookSnapshot& book) noexcept override {
        const auto idx = book.instrument.symbol().raw;
        if (!wants_depth(book.instrument, book.ts_init)) {
            return;
        }
        depth_published_ns_[idx] = book.ts_init.ns;
        ++counts_.depth;
        counts_.depth_levels = std::max({counts_.depth_levels, book.bid_count, book.ask_count});
        ch_.depth->write(idx, book);
        latency_->published(raikiri::gatewayd::Channel::Depth, book.ts_event, book.ts_init);
    }

    void on_ticker_stats(const raikiri::TickerStats& stats) noexcept override {
        ++counts_.ticker_stats;
        const auto idx = stats.instrument.symbol().raw;
        if (idx < raikiri::shm::k_ticker_cells) {
            ch_.ticker_stats->write(idx, stats);
        }
    }

    void on_funding(const raikiri::FundingUpdate& funding) noexcept override {
        ++counts_.funding;
        const auto idx = funding.instrument.symbol().raw;
        if (idx < raikiri::shm::k_funding_cells) {
            ch_.funding->write(idx, funding);
            funded_[idx] = true;
        }
        latency_->published(raikiri::gatewayd::Channel::Funding, funding.ts_event, funding.ts_init);
        // journal funding only when the rate or settlement time changes; venues restate it on a timer
        if (idx < raikiri::shm::k_funding_cells) {
            const Stated now{
                .rate = funding.rate.raw, .next = funding.next_funding_time.ns, .interval_s = funding.interval_s};
            if (stated_[idx] == now) {
                return;
            }
            stated_[idx] = now;
        }
        (void)raikiri::schema::append_msg<msgs::FundingMsg>(*journal_, funding.ts_init.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.instrument(jt::instrumentId{funding.instrument.packed});
            m.rate(jt::fixedE9{funding.rate.raw});
            m.predictedRate(jt::fixedE9{funding.predicted_rate.raw});
            m.markPrice(jt::priceTicks{funding.mark_price.raw});
            m.nextFundingTime(jt::timestampNs{funding.next_funding_time.ns});
            m.intervalS(funding.interval_s);
            m.accrual(static_cast<jt::fundingAccrualEnum>(funding.accrual));
            m.tsEvent(jt::timestampNs{funding.ts_event.ns});
            m.tsInit(jt::timestampNs{funding.ts_init.ns});
        });
    }

    void on_option_greeks(const raikiri::OptionGreeks& greeks) noexcept override {
        ++counts_.greeks;
        const auto idx = greeks.instrument.symbol().raw;
        if (idx < raikiri::shm::k_greeks_cells) {
            ch_.greeks->write(idx, greeks);
        }
    }

    // synthesized locally so the order reaches a terminal state
    void reject(const raikiri::OrderCmd& cmd, std::string_view why) noexcept {
        raikiri::OrderEvent event{};
        event.type = raikiri::OrderEventType::Rejected;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.price = cmd.price;
        event.qty = cmd.qty;
        event.leaves_qty = raikiri::QtyLots::from_raw(0);
        event.ts_event = cmd.ts_created;
        event.ts_init = cmd.ts_created;
        on_order_error(cmd.client_order_id,
                       raikiri::exec::VenueError::make(raikiri::exec::ErrorClass::Terminal, 0, why));
        on_order_event(event);
    }

    void on_order_error(raikiri::ClientOrderId client_order_id,
                        const raikiri::exec::VenueError& error) noexcept override {
        ++counts_.order_errors;
        pending_ = {client_order_id.raw, error};
        RLOG_WARN("gatewayd: refused coid={:#x} — {} (class={}, code={})",
                  client_order_id.raw,
                  error.message.view(),
                  error_class_name(error.cls),
                  error.venue_code);
    }

    // broadcast rings are single-producer: the account poller publishes charges, this thread only counts them
    void on_funding_charge(const raikiri::LedgerEntry&) noexcept override { ++counts_.funding_charges; }

    [[nodiscard]] static bool is_rejection(raikiri::OrderEventType type) noexcept {
        return type == raikiri::OrderEventType::Rejected || type == raikiri::OrderEventType::CancelRejected ||
               type == raikiri::OrderEventType::ModifyRejected;
    }

    void on_order_event(const raikiri::OrderEvent& raw) noexcept override {
        ++counts_.order_events;
        // adapters emit on_order_error immediately before the rejection event; the join relies on that adjacency
        auto event = raw;
        if (is_rejection(event.type) && pending_.first == event.client_order_id.raw) {
            event.reject_class = pending_.second.cls;
            event.reject_code = static_cast<std::int32_t>(pending_.second.venue_code);
            pending_ = {};
        }
        ch_.exec_events->publish({event});
        latency_->order_event(event);
        (void)raikiri::schema::append_msg<msgs::OrderEventMsg>(*journal_, event.ts_init.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.eventType(static_cast<jt::orderEventTypeEnum>(event.type));
            m.side(static_cast<jt::sideEnum>(event.side));
            m.orderType(static_cast<jt::orderTypeEnum>(event.order_type));
            m.tif(static_cast<jt::tifEnum>(event.tif));
            m.flags(static_cast<std::uint8_t>(event.flags));
            m.liquidity(static_cast<jt::liquidityEnum>(event.liquidity));
            m.instrument(jt::instrumentId{event.instrument.packed});
            m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
            m.exchangeClientOrderIdField().assign_range(event.exchange_client_order_id.view());
            m.exchangeOrderIdField().assign_range(event.exchange_order_id.view());
            m.execIdField().assign_range(event.exec_id.view());
            m.price(jt::priceTicks{event.price.raw});
            m.qty(jt::qtyLots{event.qty.raw});
            m.leavesQty(jt::qtyLots{event.leaves_qty.raw});
            m.fee(jt::fixedE9{event.fee.raw});
            m.tsEvent(jt::timestampNs{event.ts_event.ns});
            m.tsInit(jt::timestampNs{event.ts_init.ns});
            m.rejectClass(static_cast<jt::errorClassEnum>(event.reject_class));
            m.rejectCode(jt::venueCode{event.reject_code});
        });
    }

    struct Counts {
        std::uint64_t bbo{0};
        std::uint64_t trades{0};
        std::uint64_t depth{0};
        std::uint64_t depth_coalesced{0};
        std::uint32_t depth_levels{0};
        std::uint64_t funding{0};
        std::uint64_t ticker_stats{0};
        std::uint64_t greeks{0};
        std::uint64_t order_events{0};
        std::uint64_t order_errors{0};
        std::uint64_t funding_charges{0};
    };

    std::pair<std::uint64_t, raikiri::exec::VenueError> pending_{};

    [[nodiscard]] const Counts& counts() const noexcept { return counts_; }

    [[nodiscard]] raikiri::gatewayd::MessageCounts message_counts() const noexcept {
        return {
            counts_.bbo, counts_.trades, counts_.depth, counts_.funding, counts_.ticker_stats, counts_.greeks, 0, 0};
    }

    [[nodiscard]] raikiri::gatewayd::Coverage coverage(std::uint64_t now_ns, std::uint64_t window_ns) const noexcept {
        raikiri::gatewayd::Coverage out{};
        for (std::size_t i = 0; i < priced_ns_.size(); ++i) {
            if (priced_ns_[i] != 0) {
                ++out.priced_ever;
                out.priced_60s += now_ns - std::min(now_ns, priced_ns_[i]) <= window_ns ? 1U : 0U;
            }
            out.books_seeded += depth_published_ns_[i] != 0 ? 1U : 0U;
            out.funding_received += funded_[i] ? 1U : 0U;
        }
        return out;
    }

    void on_connectivity(const raikiri::exec::ConnEvent& event) noexcept override {
        (void)raikiri::schema::append_msg<msgs::ConnectivityMsg>(*journal_, event.ts.ns, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.venue(static_cast<jt::venueIdEnum>(venue_));
            m.event(static_cast<jt::connEventEnum>(event.state));
            m.streamId(event.stream_id);
            m.tsInit(jt::timestampNs{event.ts.ns});
        });
    }

private:
    static constexpr std::uint64_t k_depth_window_ns = 10'000'000ULL;

    raikiri::VenueId venue_;
    raikiri::shm::GatewayChannels ch_;
    raikiri::journal::JournalWriter* journal_;
    raikiri::gatewayd::LatencyMeter* latency_;

    std::array<std::uint64_t, raikiri::shm::k_depth_cells> depth_published_ns_{};

    struct Stated {
        std::int64_t rate{raikiri::FixedE9{}.raw};
        std::uint64_t next{0};
        std::uint32_t interval_s{0};

        [[nodiscard]] bool operator==(const Stated&) const noexcept = default;
    };

    std::array<Stated, raikiri::shm::k_funding_cells> stated_{};
    std::array<std::uint64_t, raikiri::shm::k_bbo_cells> priced_ns_{};
    std::array<bool, raikiri::shm::k_funding_cells> funded_{};
    std::uint64_t md_seq_{0};
    Counts counts_{};
};

} // namespace

constexpr int k_recon_interval_slices = 300; // 300 x 100 ms

// drops anything the poller's adapter emits so it is not counted twice
struct NullSink final : raikiri::exec::GatewaySink {
    void on_bbo(const raikiri::Bbo&) noexcept override {}

    void on_trade(const raikiri::TradeTick&) noexcept override {}

    void on_ticker_stats(const raikiri::TickerStats&) noexcept override {}

    void on_funding(const raikiri::FundingUpdate&) noexcept override {}

    void on_order_event(const raikiri::OrderEvent&) noexcept override {}

    void on_connectivity(const raikiri::exec::ConnEvent&) noexcept override {}
};

// a failed pull writes nothing; a successful one writes every open order and clears the rest
void publish_open_orders(const raikiri::exec::VenueAdapter& adapter,
                         raikiri::MarketType market,
                         raikiri::shm::OpenOrderTable& table,
                         raikiri::UnixNanos now) {
    char buf[16];
    const auto who = raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
    static bool announced = false;
    if (!adapter.caps.order_pull.has(market)) {
        if (!announced) {
            announced = true;
            RLOG_INFO("gatewayd[{}]: open-order reconciliation unavailable on this market", who);
        }
        return;
    }
    const auto reports = adapter.execution->open_order_reports(market);
    if (!reports.has_value()) {
        RLOG_WARN("gatewayd[{}]: open-order pull failed — {}", who, reports.error().message.view());
        return;
    }
    if (reports->size() > raikiri::shm::k_open_order_cells) {
        RLOG_ERROR("gatewayd[{}]: venue reports {} open orders, table holds {} — not published",
                   who,
                   reports->size(),
                   raikiri::shm::k_open_order_cells);
        return;
    }
    std::size_t slot = 0;
    for (const auto& report : *reports) {
        raikiri::shm::OpenOrderCell cell{};
        cell.exchange_client_order_id = report.exchange_client_order_id;
        cell.instrument = report.instrument;
        cell.leaves = report.leaves;
        cell.ts_venue = report.ts_venue;
        cell.ts_pulled = now;
        table.write(slot++, cell);
    }
    for (; slot < raikiri::shm::k_open_order_cells; ++slot) {
        table.write(slot, raikiri::shm::OpenOrderCell{});
    }
    static std::size_t said = std::numeric_limits<std::size_t>::max();
    if (said != reports->size()) {
        said = reports->size();
        RLOG_INFO("gatewayd[{}]: venue answered its open orders — {} working", who, reports->size());
    }
}

// overlap so a trade the venue reports late is not skipped; the reader dedupes by id
constexpr std::uint64_t k_fill_overlap_ns = 60ULL * 1'000'000'000ULL;

constexpr std::uint64_t k_ledger_replay_interval_ns = 600ULL * 1'000'000'000ULL;

void publish_funding_charges(const raikiri::exec::VenueAdapter& adapter,
                             raikiri::MarketType market,
                             raikiri::shm::LedgerEntryRing& ring,
                             raikiri::UnixNanos& cursor,
                             raikiri::UnixNanos& replay_due,
                             raikiri::UnixNanos now) {
    char buf[16];
    const auto who = raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
    static bool announced = false;
    if (!adapter.caps.funding_charge.has(market)) {
        if (!announced) {
            announced = true;
            RLOG_INFO("gatewayd[{}]: this venue reports no funding charges on this market", who);
        }
        return;
    }
    const auto charges = adapter.execution->funding_charges(market, cursor);
    if (!charges.has_value()) {
        RLOG_WARN("gatewayd[{}]: funding pull failed — {} (class={}, code={})",
                  who,
                  charges.error().message.view(),
                  error_class_name(charges.error().cls),
                  charges.error().venue_code);
        return;
    }
    // replay the whole window periodically for readers that attached late; the ledger drops repeats
    const bool replay = now.ns >= replay_due.ns;
    if (replay) {
        replay_due = raikiri::UnixNanos{now.ns + k_ledger_replay_interval_ns};
    }
    for (const auto& charge : *charges) {
        if (!replay && charge.settled_at.ns <= cursor.ns) {
            continue; // already published on an earlier pass
        }
        ring.publish({charge});
        RLOG_INFO("gatewayd[{}]: {} {}{}.{:09} settled at {} ref {}",
                  who,
                  charge.kind == raikiri::LedgerKind::Fee ? "fee" : "funding",
                  charge.amount.raw < 0 ? "-" : "+",
                  std::abs(charge.amount.raw) / 1'000'000'000,
                  std::abs(charge.amount.raw) % 1'000'000'000,
                  charge.settled_at.ns,
                  charge.reference.view());
    }
    for (const auto& charge : *charges) {
        cursor = raikiri::UnixNanos{std::max(cursor.ns, charge.settled_at.ns)};
    }
}

// poller fills ride their own ring because broadcast rings are single-producer
// the cursor moves off venue stamps and does not move on a failed pull
void publish_fills(const raikiri::exec::VenueAdapter& adapter,
                   raikiri::MarketType market,
                   raikiri::shm::FillRing& ring,
                   raikiri::UnixNanos& cursor) {
    char buf[16];
    const auto who = raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
    static bool announced = false;
    if (!adapter.caps.fill_pull.has(market)) {
        if (!announced) {
            announced = true;
            RLOG_INFO("gatewayd[{}]: this venue reports no trade history on this market", who);
        }
        return;
    }
    const auto fills = adapter.execution->fill_reports(market, cursor);
    if (!fills.has_value()) {
        RLOG_WARN("gatewayd[{}]: fill pull failed — {} (class={}, code={})",
                  who,
                  fills.error().message.view(),
                  error_class_name(fills.error().cls),
                  fills.error().venue_code);
        return;
    }
    for (const auto& fill : *fills) {
        ring.publish({fill});
    }
    for (const auto& fill : *fills) {
        if (fill.ts_venue.ns > cursor.ns + k_fill_overlap_ns) {
            cursor = raikiri::UnixNanos{fill.ts_venue.ns - k_fill_overlap_ns};
        }
    }
    static std::size_t said = std::numeric_limits<std::size_t>::max();
    if (said != fills->size()) {
        said = fills->size();
        RLOG_INFO("gatewayd[{}]: venue answered its trade history — {} fill(s) in the window", who, fills->size());
    }
}

void publish_positions(const raikiri::exec::VenueAdapter& adapter,
                       raikiri::MarketType market,
                       const raikiri::shm::CatalogueTable& catalogue,
                       raikiri::shm::PositionTable& table,
                       raikiri::UnixNanos now) {
    char buf[16];
    const auto who = raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
    static bool announced = false;
    static std::size_t answered = std::numeric_limits<std::size_t>::max();
    if (!adapter.caps.position_pull.has(market)) {
        if (!announced) {
            announced = true;
            RLOG_INFO("gatewayd[{}]: position reconciliation unavailable on this market", who);
        }
        return;
    }

    const auto reports = adapter.execution->position_reports(market);
    if (!reports.has_value()) {
        // publish nothing on failure so the cells age out to unknown
        RLOG_WARN("gatewayd[{}]: position pull failed — {}", who, reports.error().message.view());
        return;
    }

    if (answered != reports->size()) {
        answered = reports->size();
        RLOG_INFO("gatewayd[{}]: venue answered its positions — {} open", who, reports->size());
    }

    std::unordered_map<std::uint64_t, raikiri::exec::PositionReport> by_instrument;
    for (const auto& report : *reports) {
        by_instrument[report.instrument.packed] = report;
    }
    for (const auto& listed : catalogue.all()) {
        const auto id = raikiri::base_of(listed).id;
        const auto idx = id.symbol().raw;
        if (idx >= raikiri::shm::k_position_cells) {
            continue;
        }
        raikiri::shm::PositionCell cell{};
        if (const auto it = by_instrument.find(id.packed); it != by_instrument.end()) {
            cell.report = it->second;
        } else {
            // a default QtyLots is undef, so set every field for a flat position
            cell.report.instrument = id;
            cell.report.net_qty = raikiri::QtyLots::from_raw(0);
            cell.report.avg_entry = raikiri::PriceTicks::from_raw(0);
            cell.report.ts_venue = now;
        }
        cell.ts_pulled = now;
        table.write(idx, cell);
    }
}

// the balance set is one seqlock write; a failed pull writes nothing
void publish_balances(const raikiri::exec::VenueAdapter& adapter,
                      raikiri::MarketType market,
                      raikiri::shm::BalanceTable& table,
                      raikiri::UnixNanos now) {
    std::array<char, 16> buf{};
    const auto who = raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf.data(), buf.size());
    static bool announced = false;
    static std::uint32_t answered = std::numeric_limits<std::uint32_t>::max();
    if (!adapter.caps.balance_pull.has(market)) {
        if (!announced) {
            announced = true;
            RLOG_INFO("gatewayd[{}]: balances unavailable on this market", who);
        }
        return;
    }

    const auto reports = adapter.execution->balance_reports(market);
    if (!reports.has_value()) {
        RLOG_WARN("gatewayd[{}]: balance pull failed — {}", who, reports.error().message.view());
        return;
    }
    if (reports->size() > raikiri::shm::k_balance_rows) {
        RLOG_ERROR("gatewayd[{}]: {} assets exceeds the {} the snapshot holds",
                   who,
                   reports->size(),
                   raikiri::shm::k_balance_rows);
        return;
    }

    raikiri::shm::BalanceSnapshot snapshot{};
    snapshot.count = static_cast<std::uint32_t>(reports->size());
    snapshot.ts_pulled = now;
    for (std::size_t i = 0; i < reports->size(); ++i) {
        snapshot.rows[i] = (*reports)[i];
    }
    table.write(0, snapshot);

    if (answered != snapshot.count) {
        answered = snapshot.count;
        RLOG_INFO("gatewayd[{}]: venue answered its balances — {} asset(s)", who, snapshot.count);
    }
}

// last rate-limit header values seen; locked because the poller thread also writes
class RateWitness {
public:
    void observe(std::string_view host, const raikiri::net::RateHeader& header, raikiri::UnixNanos at) {
        const std::lock_guard lock{mu_};
        seen_[std::string{host} + " " + header.name] = Seen{.value = header.value, .at = at};
    }

    void report(std::string_view account) const {
        const std::lock_guard lock{mu_};
        if (seen_.empty()) {
            return; // a venue that states nothing must not be reported as zero
        }
        for (const auto& [key, seen] : seen_) {
            RLOG_INFO(
                "gatewayd[{}]: venue's own metering — {} = {} (read at {})", account, key, seen.value, seen.at.ns);
        }
    }

private:
    struct Seen {
        std::string value;
        raikiri::UnixNanos at{};
    };

    mutable std::mutex mu_;
    std::map<std::string, Seen> seen_;
};

// per-endpoint request tally printed at shutdown; RAIKIRI_LOG_REQUESTS=1 logs each request
class RequestLedger {
public:
    // venue symbols are upper case and endpoint names are not; api versions like v1 must survive
    [[nodiscard]] static bool names_an_instrument(std::string_view segment) noexcept {
        if (segment.size() < 3) {
            return false; // `v1`, `q`, and anything else too short to be a symbol
        }
        return std::ranges::none_of(segment, [](unsigned char c) { return std::islower(c) != 0; });
    }

    [[nodiscard]] static std::string collapse(std::string_view path) {
        std::string out;
        out.reserve(path.size());
        std::size_t at = 0;
        while (at <= path.size()) {
            const auto end = std::min(path.find('/', at), path.size());
            const auto segment = path.substr(at, end - at);
            out.append(names_an_instrument(segment) ? std::string_view{"{symbol}"} : segment);
            if (end == path.size()) {
                break;
            }
            out.push_back('/');
            at = end + 1;
        }
        return out;
    }

    RequestLedger() : verbose_{std::getenv("RAIKIRI_LOG_REQUESTS") != nullptr} {}

    void observe(std::string_view host,
                 std::string_view target,
                 int status,
                 std::uint64_t elapsed_ns,
                 std::string_view account) {
        // count by path shape, with instrument segments collapsed
        const auto path = collapse(target.substr(0, target.find('?')));
        const std::lock_guard lock{mu_};
        auto key = std::string{host}.append(path);
        book_.series(key, raikiri::introspect::Stage::Rest).record(static_cast<std::int64_t>(elapsed_ns));
        auto& row = spent_[std::move(key)];
        ++row.calls;
        if (status < 200 || status >= 300) {
            ++row.refused;
            row.last_status = status;
        }
        if (verbose_) {
            RLOG_INFO("gatewayd[{}]: {} {} -> {} in {} ms", account, host, target, status, elapsed_ns / 1'000'000);
        }
    }

    void arrival(std::string_view host, std::uint64_t queued_ns) {
        const std::lock_guard lock{mu_};
        book_.series(host, raikiri::introspect::Stage::KernelQueue).record(static_cast<std::int64_t>(queued_ns));
    }

    void pong(std::string_view host, std::uint64_t rtt_ns) {
        const std::lock_guard lock{mu_};
        book_.series(host, raikiri::introspect::Stage::WsPing).record(static_cast<std::int64_t>(rtt_ns));
    }

    [[nodiscard]] std::vector<raikiri::introspect::LatencyStats> latency(std::uint64_t mono_ns) {
        const std::lock_guard lock{mu_};
        book_.rotate_if_due(mono_ns);
        return book_.stats();
    }

    [[nodiscard]] std::vector<raikiri::introspect::RestEndpointStats> endpoints() const {
        const std::lock_guard lock{mu_};
        std::vector<raikiri::introspect::RestEndpointStats> out;
        out.reserve(spent_.size());
        for (const auto& [endpoint, row] : spent_) {
            out.push_back(
                {.endpoint = endpoint, .calls = row.calls, .refused = row.refused, .last_status = row.last_status});
        }
        return out;
    }

    void report(std::string_view account) const {
        const std::lock_guard lock{mu_};
        for (const auto& [endpoint, row] : spent_) {
            if (row.refused != 0) {
                RLOG_WARN("gatewayd[{}]: {} — {} calls, {} REFUSED (last status {})",
                          account,
                          endpoint,
                          row.calls,
                          row.refused,
                          row.last_status);
            } else {
                RLOG_INFO("gatewayd[{}]: {} — {} calls", account, endpoint, row.calls);
            }
        }
    }

private:
    struct Row {
        std::uint64_t calls{0};
        std::uint64_t refused{0};
        int last_status{0};
    };

    mutable std::mutex mu_;
    std::map<std::string, Row> spent_;
    raikiri::introspect::LatencyBook book_;
    bool verbose_;
};

constexpr std::string_view
    k_usage = R"(usage: gatewayd --venue=bybit [--market=spot|perp] [--symbol=BTCUSDT] [--duration <sec>]
                [--channels=bbo,trades,depth,funding] [--feed=<adapter feed>] [--testnet]
                [--api-key=<ref>] [--api-secret=<ref>] [--passphrase=<ref>]
)";

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (const std::string_view arg{argv[i]}; arg == "--help" || arg == "-h") {
            std::fputs(k_usage.data(), stdout);
            return 0;
        }
    }
    // everything logs to stderr; stdout is not line buffered
    const raikiri::log::Scope logging{"gatewayd"};
    using namespace raikiri;

    std::string venue_name = "sim";
    std::uint64_t duration_s = 0; // 0 = run until signaled
    bool testnet = false;
    std::string api_key_ref;
    std::string passphrase_ref;
    std::string api_secret_ref;
    std::vector<std::string> symbols;
    std::string market_arg;
    SubKinds wanted_channels{}; // all on
    std::string channels_arg;
    std::optional<std::string> feed_arg; // wins over venues.yaml's market_data_feed
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg.starts_with("--venue=")) {
            venue_name = arg.substr(8);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--testnet") {
            testnet = true;
        } else if (arg.starts_with("--market=")) {
            market_arg = arg.substr(9);
        } else if (arg.starts_with("--channels=")) {
            channels_arg = arg.substr(11);
        } else if (arg.starts_with("--feed=")) {
            feed_arg = std::string{arg.substr(7)};
        } else if (arg.starts_with("--symbol=")) {
            symbols.emplace_back(arg.substr(9));
        } else if (arg.starts_with("--api-key=")) {
            // a credential reference such as env:NAME, file:/path or exec:cmd
            api_key_ref = arg.substr(10);
        } else if (arg.starts_with("--api-secret=")) {
            api_secret_ref = arg.substr(13);
        } else if (arg.starts_with("--passphrase=")) {
            passphrase_ref = arg.substr(13);
        }
    }
    if (!channels_arg.empty()) {
        const auto parsed = parse_channels(channels_arg);
        if (!parsed.has_value()) {
            fmt::print(stderr, "gatewayd: --channels: {}\n", parsed.error());
            return 2;
        }
        wanted_channels = *parsed;
    }
    const auto venue = venue_from_string(venue_name);
    if (venue == VenueId::Invalid) {
        fmt::print(stderr, "gatewayd: unknown --venue={}\n", venue_name);
        return 2;
    }
    if (!api_key_ref.empty()) {
        config::disable_core_dumps();
    }
    // logs where each credential comes from, never its value
    const auto report_credential = [](std::string_view what, const std::string& spec) {
        if (spec.empty()) {
            return true;
        }
        const auto ref = config::SecretRef::parse(spec);
        if (!ref.has_value()) {
            RLOG_ERROR("gatewayd: {} reference is malformed ({})", what, config::to_string(ref.error()));
            return false;
        }
        RLOG_INFO("gatewayd: {} from {}", what, config::to_string(ref->scheme));
        if (ref->scheme == config::SecretRef::Scheme::OnePassword) {
            const auto status = config::probe_one_password();
            if (!status.usable()) {
                RLOG_ERROR("gatewayd: 1password unusable — {}", status.diagnostic);
                RLOG_ERROR("gatewayd: on a server set OP_SERVICE_ACCOUNT_TOKEN; on a laptop "
                           "run `eval $(op signin)`");
                return false;
            }
            RLOG_INFO("gatewayd: 1password {} as {}", config::to_string(status.auth), status.account);
        }
        if (config::is_plaintext(ref->scheme)) {
            RLOG_WARN("gatewayd: {} is a literal value in configuration. Fine for a demo; "
                      "for anything holding real funds use env:, file: or exec: (e.g. "
                      "exec:op read op://vault/item/field).",
                      what);
        }
        return true;
    };
    if (!report_credential("api key", api_key_ref) || !report_credential("api secret", api_secret_ref) ||
        !report_credential("passphrase", passphrase_ref)) {
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;

    const auto& caps = exec::caps_of(venue);
    MarketType market = MarketType::Perp;
    if (market_arg == "spot") {
        market = MarketType::Spot;
    } else if (market_arg == "future") {
        market = MarketType::Future;
    } else if (market_arg == "option") {
        market = MarketType::Option;
    } else if (market_arg == "perp" || market_arg.empty()) {
        market = caps.markets.perp ? MarketType::Perp : MarketType::Spot;
    } else {
        fmt::print(stderr, "gatewayd: --market must be spot, perp, future or option\n");
        return 2;
    }
    if (!caps.serves(market)) {
        RLOG_ERROR("gatewayd[{}]: this adapter does not serve the {} market", venue_name, to_string(market));
        return 2;
    }

    const AccountId account{.venue = venue, .market = market};
    char account_buf[16];
    const auto account_name = std::string{format_account(account, account_buf, sizeof account_buf)};

    const auto seg_path = shm::gateway_segment_path(account);
    if (!journal::internal::mkdir_p(platform::runtime_dir() + "/shm")) {
        RLOG_ERROR("gatewayd: cannot create runtime dir");
        return 1;
    }
    const auto generation = clock.mono().ns;
    const auto started_ns = clock.wall().ns;
    auto owner = shm::SegmentOwner::create(seg_path, shm::k_gateway_segment_bytes, generation, generation);
    if (!owner.has_value()) {
        RLOG_ERROR("gatewayd: segment create failed ({})", static_cast<int>(owner.error()));
        return 1;
    }
    auto channels = shm::create_gateway_channels(*owner);
    if (!channels.has_value()) {
        return 1;
    }

    const char* data_dir_env = std::getenv("RAIKIRI_DATA_DIR");
    const std::string data_dir = data_dir_env != nullptr ? data_dir_env : "raikiri-data";
    // keep eight journal segments (2 GiB) per account; RAIKIRI_JOURNAL_RETAIN=0 keeps all
    constexpr std::size_t k_retain_segments = 8;
    auto retain = k_retain_segments;
    if (const char* env = std::getenv("RAIKIRI_JOURNAL_RETAIN"); env != nullptr && *env != '\0') {
        retain = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
    }
    auto journal_writer = journal::JournalWriter::open(
        data_dir + "/journal/gatewayd-" + account_name, "journal", journal::k_default_segment_bytes, retain);
    if (!journal_writer.has_value()) {
        RLOG_ERROR("gatewayd[{}]: journal open failed{}",
                   account_name,
                   journal_writer.error() == journal::JournalError::AlreadyWriting
                       ? " — another process is already writing it"
                       : "");
        return 1;
    }

    introspect::LatencyBook feed_latency;
    gatewayd::LatencyMeter latency{feed_latency, clock};
    ShmSink sink{venue, *channels, *journal_writer, latency};

    venues::sim::register_adapter();
    venues::bybit::register_adapter();
    venues::binance::register_adapter();
    venues::bitget::register_adapter();
    venues::htx::register_adapter();
    venues::bitfinex::register_adapter();
    venues::deribit::register_adapter();
    venues::hyperliquid::register_adapter();
    venues::coinbase::register_adapter();
    venues::kraken::register_adapter();
    venues::gate::register_adapter();
    venues::kucoin::register_adapter();
    venues::bingx::register_adapter();
    venues::xt::register_adapter();
    venues::mexc::register_adapter();
    venues::okx::register_adapter();
    venues::weex::register_adapter();
    venues::aster::register_adapter();
    venues::grvt::register_adapter();
    venues::lighter::register_adapter();
    venues::backpack::register_adapter();
    venues::cryptocom::register_adapter();
    venues::blofin::register_adapter();
    venues::apex::register_adapter();
    venues::edgex::register_adapter();
    venues::dydx::register_adapter();
    venues::phemex::register_adapter();
    venues::extended::register_adapter();
    venues::pacifica::register_adapter();
    venues::lbank::register_adapter();
    venues::bitunix::register_adapter();
    venues::toobit::register_adapter();
    // one budget shared with the account poller; a venue missing from the file is unlimited
    std::optional<exec::RateBudget> budget;
    // an absent venue still gets the default answer timeout; zero would never report an unanswered order
    std::uint64_t answer_timeout_ns = config::VenueSettings{}.order_answer_timeout_ms * 1'000'000ULL;
    std::uint32_t book_depth = 0; // the adapter's own tier until a file says otherwise
    std::string market_data_feed; // and the adapter's own feed
    const auto venues_path = config::config_dir() + "/venues.yaml";
    if (const auto settings = config::load_venues(venues_path); settings.has_value()) {
        if (const auto it = settings->venues.find(std::string{venue_name}); it != settings->venues.end()) {
            if (const auto limits = config::rest_limits(it->second); !limits.empty()) {
                budget.emplace(limits, config::rest_costs(it->second), it->second.default_cost);
            }
            net::set_process_ws_limits(
                net::ws_limits(venue_name, it->second, market == MarketType::Spot ? "spot" : "perp"));
            answer_timeout_ns = it->second.order_answer_timeout_ms * 1'000'000ULL;
            book_depth = it->second.book_depth;
            market_data_feed = it->second.market_data_feed;
        } else {
            RLOG_INFO("gatewayd[{}]: no rate budget configured — running unlimited", account_name);
        }
    } else {
        RLOG_WARN("gatewayd: {} — running unlimited", settings.error());
    }

    if (feed_arg.has_value()) {
        market_data_feed = *feed_arg;
    }
    auto adapter = exec::make_venue({.venue = venue,
                                     .market = market,
                                     .testnet = testnet,
                                     .api_key_env = api_key_ref,
                                     .api_secret_env = api_secret_ref,
                                     .passphrase_env = passphrase_ref,
                                     .budget = budget.has_value() ? &*budget : nullptr,
                                     .order_answer_timeout_ns = answer_timeout_ns,
                                     .book_depth = book_depth,
                                     .market_data_feed = market_data_feed},
                                    sink,
                                    clock);
    if (!adapter.has_value()) {
        RLOG_ERROR("gatewayd: adapter build failed — {}", exec::to_string(adapter.error()));
        return 1;
    }
    if (!adapter->credential_errors.empty()) {
        for (const auto& error : adapter->credential_errors) {
            RLOG_ERROR("gatewayd: credential reference did not resolve — {}", error);
        }
        return 1;
    }
    if (const auto started = adapter->data->start(); !started.has_value()) {
        RLOG_ERROR("gatewayd: market data failed to start — {}", started.error().message.view());
        return 1;
    }
    // without credentials order entry refuses; command rings are drained only once execution is up
    const bool credentials_configured = !api_key_ref.empty() && !api_secret_ref.empty();
    bool can_trade = false;
    if (const auto started = adapter->execution->start(); started.has_value()) {
        can_trade = true;
    } else if (credentials_configured) {
        RLOG_ERROR("gatewayd: order entry failed to start — {}", started.error().message.view());
        return 1;
    } else {
        RLOG_INFO("gatewayd[{}]: market data only — {}", account_name, started.error().message.view());
    }
    // load the catalogue before subscribing; adapters need tick and lot sizes to map frames
    std::vector<Instrument> catalogue;
    if (venue == VenueId::Sim) {
        catalogue = demo::sim_instruments();
        for (auto& instrument : catalogue) {
            classify(venue, instrument);
        }
    } else {
        const auto load_deadline = duration_s != 0 ? clock.mono() + duration_s * 1'000'000'000ULL : MonoNanos{~0ULL};
        const auto stopping = [&] { return g_stop.load(std::memory_order_relaxed) || clock.mono() >= load_deadline; };
        auto loaded = exec::load_catalogue(
            *adapter->instruments,
            market,
            stopping,
            [&](const exec::VenueError& error, std::uint32_t attempt, std::chrono::milliseconds wait) {
                RLOG_WARN("gatewayd[{}]: catalogue load failed (attempt {}): "
                          "{} (code {}) — retrying in {} ms",
                          account_name,
                          attempt,
                          error.message.view(),
                          error.venue_code,
                          wait.count());
            });
        if (!loaded.has_value()) {
            if (stopping()) {
                return 0;
            }
            RLOG_ERROR("gatewayd[{}]: catalogue load failed: {} (code {})",
                       account_name,
                       loaded.error().message.view(),
                       loaded.error().venue_code);
            return 1;
        }
        catalogue = std::move(*loaded);
    }
    RLOG_INFO("gatewayd[{}]: {} instruments loaded", account_name, catalogue.size());

    // publish the numbering before the segment goes ready
    for (const auto& instrument : catalogue) {
        if (const auto pushed = channels->catalogue->push(instrument); !pushed.has_value()) {
            RLOG_ERROR("gatewayd[{}]: catalogue publish refused symbol {}",
                       account_name,
                       base_of(instrument).exchange_symbol.view());
            return 1;
        }
    }

    const auto loaded_at = clock.wall().ns;
    for (const auto& instrument : catalogue) {
        const auto& base = base_of(instrument);
        (void)schema::append_msg<msgs::InstrumentMsg>(*journal_writer, loaded_at, [&](auto m) {
            namespace jt = raikiri_journal::types;
            m.instrument(jt::instrumentId{base.id.packed});
            m.generation(generation);
            m.symbol().assign_range(base.exchange_symbol.view());
            m.localSymbol().assign_range(base.local_symbol.view());
            m.kind(static_cast<jt::contractKindEnum>(base.kind));
            m.tickSize(jt::fixedE9{base.tick_size.raw});
            m.lotSize(jt::fixedE9{base.lot_size.raw});
            m.minNotional(jt::fixedE9{base.min_notional.raw});
            m.contractValue(jt::fixedE9{base.contract_value.raw});
            m.tsLoaded(jt::timestampNs{loaded_at});
            m.assetClass(static_cast<jt::assetClassEnum>(base.asset_class));
            m.underlying().assign_range(base.underlying.view());
        });
    }

    std::size_t matched = 0;
    std::size_t subscribed = 0;
    std::string first_refusal;
    gatewayd::SubTallies sub_tallies{};
    const auto count_sub = [&](gatewayd::Channel channel, bool ok) {
        auto& row = sub_tallies[static_cast<std::size_t>(channel)];
        ++row.requested;
        ++(ok ? row.accepted : row.refused);
        return ok;
    };
    for (const auto& instrument : catalogue) {
        const auto& base = base_of(instrument);
        const auto wanted = symbols.empty() || std::any_of(symbols.begin(), symbols.end(), [&](const std::string& s) {
                                return s == base.exchange_symbol.view();
                            });
        if (!wanted) {
            continue;
        }
        ++matched;
        // only the bbo result decides whether an instrument counts as subscribed
        const auto bbo = adapter->data->subscribe(exec::SubKind::Bbo, base.id);
        if (!count_sub(gatewayd::Channel::Bbo, bbo.has_value())) {
            if (first_refusal.empty()) {
                first_refusal = std::string{bbo.error().message.view()};
            }
            continue;
        }
        ++subscribed;
        if (wanted_channels.trades) {
            (void)count_sub(gatewayd::Channel::Trades,
                            adapter->data->subscribe(exec::SubKind::Trades, base.id).has_value());
        }
        if (wanted_channels.depth) {
            (void)count_sub(gatewayd::Channel::Depth,
                            adapter->data->subscribe(exec::SubKind::Depth, base.id).has_value());
        }
        if (wanted_channels.funding && venue != VenueId::Sim) {
            (void)count_sub(gatewayd::Channel::Funding,
                            adapter->data->subscribe(exec::SubKind::Funding, base.id).has_value());
        }
    }
    if (matched == 0) {
        RLOG_ERROR("gatewayd[{}]: no requested symbol matched the catalogue", account_name);
        return 1;
    }
    if (subscribed == 0) {
        RLOG_ERROR("gatewayd[{}]: every subscription was refused — {}",
                   account_name,
                   first_refusal.empty() ? "no reason given" : first_refusal);
        return 1;
    }
    if (subscribed < matched) {
        RLOG_WARN("gatewayd[{}]: {} of {} instruments were REFUSED a subscription — {}",
                  account_name,
                  matched - subscribed,
                  matched,
                  first_refusal.empty() ? "no reason given" : first_refusal);
    }
    RLOG_INFO("gatewayd[{}]: subscribed to {} instrument(s)", account_name, subscribed);
    owner->publish();
    RequestLedger requests;
    raikiri::net::set_request_observer(
        [&requests,
         &account_name](std::string_view host, std::string_view target, int status, std::uint64_t elapsed_ns) {
            requests.observe(host, target, status, elapsed_ns, account_name);
        });
    raikiri::net::set_pong_observer(
        [&requests](std::string_view host, std::uint64_t rtt_ns) { requests.pong(host, rtt_ns); });
    raikiri::net::set_arrival_observer(
        [&requests](std::string_view host, std::uint64_t queued_ns) { requests.arrival(host, queued_ns); });
    RateWitness rates;
    raikiri::net::set_rate_observer([&rates, &clock](std::string_view host, const raikiri::net::RateHeader& header) {
        rates.observe(host, header, clock.wall());
    });

    RLOG_INFO("gatewayd[{}]: up, segment {}", account_name, seg_path);

    introspect::FileStatsPublisher stats_files{introspect::stats_dir(platform::runtime_dir())};
    introspect::GatewayStats identity{};
    identity.account = account_name;
    identity.venue = std::string{venue_name};
    identity.market = std::string{to_string(market)};
    identity.feed = market_data_feed;
    identity.channels.emplace_back("bbo");
    for (const auto& [on, name] : {std::pair{wanted_channels.trades, "trades"},
                                   std::pair{wanted_channels.depth, "depth"},
                                   std::pair{wanted_channels.funding, "funding"}}) {
        if (on) {
            identity.channels.emplace_back(name);
        }
    }
    identity.process = {.pid = static_cast<std::int64_t>(::getpid()),
                        .generation = generation,
                        .started_ns = started_ns,
                        .uptime_s = 0,
                        .build = RAIKIRI_BUILD_ID};
    gatewayd::StatsCollector stats{std::move(identity), stats_files};
    stats.catalogue(catalogue);
    stats.subscriptions(sub_tallies);
    stats.limits(net::process_ws_limits());

    shm::SpscConsumer creates{channels->cmd_create};
    shm::SpscConsumer cancels{channels->cmd_cancel};

    // the poller gets its own adapter because adapters are single-threaded
    // execution->start() syncs the clock the pulls are signed with; data->start() is skipped
    std::unique_ptr<exec::VenueAdapter> poller_adapter;
    NullSink poller_sink;
    std::thread poller;
    // outside the block because the poller thread captures it by reference; zero asks for the venue's whole window
    raikiri::UnixNanos funding_cursor{};
    raikiri::UnixNanos ledger_replay_due{};
    raikiri::UnixNanos fill_cursor{};
    const bool private_pulls = gatewayd::private_pulls_enabled(venue, can_trade, credentials_configured);
    if (can_trade && !private_pulls) {
        RLOG_INFO("gatewayd[{}]: position, balance, open-order, funding and fill pulls off — "
                  "no credentials configured",
                  account_name);
    }
    if (private_pulls) {
        auto made = exec::make_venue(
            {.venue = venue,
             .market = market,
             .testnet = testnet,
             .api_key_env = api_key_ref,
             .api_secret_env = api_secret_ref,
             .passphrase_env = passphrase_ref,
             .budget = budget.has_value() ? &*budget : nullptr,
             .order_answer_timeout_ns = 0,
             // gcc errors on a skipped trailing designated initializer (-Wmissing-field-initializers)
             .book_depth = 0,
             .market_data_feed = {}},
            poller_sink,
            clock);
        if (made.has_value()) {
            poller_adapter = std::make_unique<exec::VenueAdapter>(std::move(*made));
            if (const auto up = poller_adapter->execution->start(); !up.has_value()) {
                RLOG_WARN("gatewayd: account poller could not start — {}", up.error().message.view());
                poller_adapter.reset();
            }
            // the poller adapter needs its own catalogue or every reported position maps to nothing
            if (poller_adapter != nullptr) {
                if (const auto loaded = poller_adapter->instruments->fetch_all(market); !loaded.has_value()) {
                    RLOG_WARN("gatewayd: account poller has no catalogue — {}", loaded.error().message.view());
                    poller_adapter.reset();
                }
            }
        }
        if (poller_adapter != nullptr) {
            poller = std::thread([&] {
                auto* channels_ptr = &*channels;
                while (!g_stop.load(std::memory_order_relaxed)) {
                    publish_positions(
                        *poller_adapter, market, *channels_ptr->catalogue, *channels_ptr->positions, clock.wall());
                    publish_balances(*poller_adapter, market, *channels_ptr->balances, clock.wall());
                    publish_open_orders(*poller_adapter, market, *channels_ptr->open_orders, clock.wall());
                    publish_funding_charges(*poller_adapter,
                                            market,
                                            *channels_ptr->funding_charges,
                                            funding_cursor,
                                            ledger_replay_due,
                                            clock.wall());
                    publish_fills(*poller_adapter, market, *channels_ptr->fills, fill_cursor);
                    for (int i = 0; i < k_recon_interval_slices && !g_stop.load(std::memory_order_relaxed); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{100});
                    }
                }
            });
        }
    }

    // probe the venue clock off the loop thread
    std::mutex probed_mu;
    introspect::ClockEstimate probed{};
    std::string probe_endpoint;
    std::thread prober;
    if (const auto endpoint = introspect::time_endpoint(venue_name, to_string(market)); endpoint.has_value()) {
        probe_endpoint = std::string{endpoint->host} + std::string{endpoint->path};
        prober = std::thread([&, endpoint = *endpoint] {
            introspect::RestServerTime source{endpoint, clock};
            std::mt19937_64 rng{std::random_device{}()};
            introspect::ProbeClock probe{source,
                                         [&clock] { return clock.mono().ns; },
                                         [&budget, &clock, path = endpoint.path] {
                                             return !budget.has_value() ||
                                                    budget->take(path, clock.mono(), exec::RateClass::Poll).has_value();
                                         },
                                         [&rng](std::uint64_t lo, std::uint64_t hi) {
                                             return std::uniform_int_distribution<std::uint64_t>{lo, hi}(rng);
                                         }};
            while (!g_stop.load(std::memory_order_relaxed)) {
                const auto due = probe.tick();
                {
                    const std::lock_guard lock{probed_mu};
                    probed = probe.estimate();
                }
                while (!g_stop.load(std::memory_order_relaxed) && clock.mono().ns < due) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{200});
                }
            }
        });
    } else {
        RLOG_INFO("gatewayd[{}]: no time endpoint — clock offset bounded from the feed", account_name);
    }

    const auto t_end = duration_s != 0 ? clock.mono() + duration_s * 1'000'000'000ULL : MonoNanos{~0ULL};
    std::uint64_t flush_due = 0;
    // spin while commands arrive, then park; without credentials wait in the reactor instead
    // 5 ms timeout keeps binance depth seeding (25 ms) and htx subscribe pacing (20 ms) at their rate
    constexpr std::uint32_t k_spin_passes = 64;
    constexpr std::uint64_t k_park_ns = 1'000'000;
    constexpr std::uint32_t k_wait_ms = 5;
    std::uint32_t idle_passes = 0;
    constexpr std::uint64_t k_stats_every_ns = 5ULL * 1'000'000'000ULL;
    std::uint64_t stats_due = 0;
    std::uint64_t stats_written = 0;
    std::uint64_t stats_cost_ns = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto mono = clock.mono();
        if (mono >= t_end) {
            // set the flag; the account poller exits on it and the join waits for it
            g_stop.store(true, std::memory_order_relaxed);
            break;
        }
        // cancels drain first
        OrderCmd cmd{};
        std::uint32_t commands = 0;
        while (cancels.try_pop(cmd)) {
            ++commands;
            if (!can_trade) {
                sink.reject(cmd, "venue is running market-data only");
                continue;
            }
            adapter->execution->cancel(cmd);
        }
        while (creates.try_pop(cmd)) {
            ++commands;
            // rejected rather than dropped so the order reaches a terminal state
            if (!can_trade) {
                sink.reject(cmd, "venue is running market-data only");
                continue;
            }
            switch (cmd.kind) {
            case CmdKind::Create:
                latency.order_sent(cmd.client_order_id.raw);
                adapter->execution->submit(cmd);
                break;
            case CmdKind::Amend:
                adapter->execution->amend(cmd);
                break;
            case CmdKind::Cancel:
                adapter->execution->cancel(cmd);
                break;
            }
        }
        adapter->data->poll(clock.wall());
        adapter->execution->poll(clock.wall());
        owner->beat(mono.ns);
        if (mono.ns >= flush_due) {
            journal_writer->flush();
            owner->publish_refusals(adapter->data->subscribe_refusals());
            flush_due = mono.ns + 50'000'000; // 50 ms group commit
        }
        if (mono.ns >= stats_due) {
            stats_due = mono.ns + k_stats_every_ns;
            const auto began = clock.mono().ns;
            const auto wall = clock.wall().ns;
            auto estimate = [&] {
                const std::lock_guard lock{probed_mu};
                return probed;
            }();
            if (estimate.method == introspect::ClockMethod::None) {
                estimate = latency.bound(wall);
            }
            latency.use_offset(estimate);
            latency.rotate_if_due(mono.ns);
            feed_latency.rotate_if_due(mono.ns);
            auto measured = feed_latency.stats();
            for (auto& row : requests.latency(mono.ns)) {
                measured.push_back(std::move(row));
            }
            const bool written = stats.publish(
                {.wall_ns = wall,
                 .mono_ns = mono.ns,
                 .messages = sink.message_counts(),
                 .coverage = sink.coverage(wall, gatewayd::StatsCollector::k_window_ns),
                 .venue_refusals = adapter->data->subscribe_refusals(),
                 .rest_endpoints = requests.endpoints(),
                 .clock = introspect::clock_stats(std::string{venue_name},
                                                  estimate.method == introspect::ClockMethod::RestProbe ? probe_endpoint
                                                                                                        : std::string{},
                                                  estimate),
                 .latency = std::move(measured)},
                budget.has_value() ? &*budget : nullptr);
            const auto cost = clock.mono().ns - began;
            stats_cost_ns += cost;
            if (written && stats_written++ == 0) {
                RLOG_INFO("gatewayd[{}]: stats every 5 s to {}/{}.json — {} bytes in {} us",
                          account_name,
                          introspect::stats_dir(platform::runtime_dir()),
                          account_name,
                          stats_files.last_bytes(),
                          cost / 1000);
            }
        }
        idle_passes = commands != 0 ? 0 : idle_passes + 1;
        if (idle_passes > k_spin_passes) {
            if (can_trade) {
                std::this_thread::sleep_for(std::chrono::nanoseconds{k_park_ns});
            } else {
                adapter->data->wait(k_wait_ms);
            }
        }
    }

    if (poller.joinable()) {
        poller.join();
    }
    if (prober.joinable()) {
        prober.join();
    }
    adapter->data->stop();
    adapter->execution->stop();
    journal_writer->flush();
    const auto& counts = sink.counts();
    requests.report(account_name);
    rates.report(account_name);
    if (stats_written != 0) {
        RLOG_INFO("gatewayd[{}]: {} stats snapshots, {} us each on average",
                  account_name,
                  stats_written,
                  stats_cost_ns / stats_written / 1000);
    }
    RLOG_INFO("gatewayd[{}]: down — bbo={} depth={}({} levels) trades={} funding={} greeks={} "
              "ticker={} charges={} order-events={} refusals={} journal-pruned={} "
              "depth-coalesced={}",
              account_name,
              counts.bbo,
              counts.depth,
              counts.depth_levels,
              counts.trades,
              counts.funding,
              counts.greeks,
              counts.ticker_stats,
              counts.funding_charges,
              counts.order_events,
              counts.order_errors,
              journal_writer->pruned(),
              counts.depth_coalesced);
    return 0;
}
