// gatewayd — one venue AND ONE MARKET per process.
//
//   gatewayd --venue=bybit [--market=spot|perp] [--symbol=BTCUSDT] [--duration <sec>]
//            [--channels=bbo,trades,depth,funding]
//            [--api-key=<ref>] [--api-secret=<ref>] [--passphrase=<ref>]
//
// Every adapter is single-market by construction — several collapse the two
// market flags into one MarketType and never look again — so a second market
// is a second process, not a second code path. The account name carries the
// market and names everything this process owns: segment "gw.bybit-spot",
// journal "gatewayd-bybit-spot". Perp is unsuffixed, so the perpetual gateway
// keeps the paths it always had.
//
// Owns the venue's shm segment, hosts the adapter, and runs the hot loop:
//   drain cancel ring -> drain create ring -> adapter poll -> heartbeat.
// Cancels always drain first (an escape hatch must never queue behind new
// risk). Every order event is journaled and published to the exec ring; BBO
// goes to the seqlock table; trades to the md ring.
//
// The demo loop is a tight spin (one core) — real deployments pin it; the
// demo just runs it unpinned for a few seconds.

#include <raikiri/common/platform.hpp>
#include <raikiri/config/one_password.hpp>
#include <raikiri/config/secret.hpp>
#include <raikiri/config/settings.hpp>
#include <raikiri/domain/demo_instruments.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/journal/detail.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/schema/codec.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/venues/binance/binance_venue.hpp>
#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/blofin/blofin_venue.hpp>
#include <raikiri/venues/bybit/bybit_venue.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>
#include <raikiri/venues/dydx/dydx_venue.hpp>
#include <raikiri/venues/extended/extended_venue.hpp>
#include <raikiri/venues/gate/gate_venue.hpp>
#include <raikiri/venues/grvt/grvt_venue.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/lighter/lighter_venue.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/okx/okx_venue.hpp>
#include <raikiri/venues/phemex/phemex_venue.hpp>
#include <raikiri/venues/sim/sim_venue.hpp>
#include <raikiri/venues/weex/weex_venue.hpp>
#include <raikiri/venues/apex/apex_venue.hpp>
#include <raikiri/venues/aster/aster_venue.hpp>
#include <raikiri/venues/backpack/backpack_venue.hpp>
#include <raikiri/venues/cryptocom/cryptocom_venue.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <expected>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// Which market-data channels this process asks the venue for.
//
// The quote is not optional and is not represented here: an adapter with no
// top of book has no market data at all, and it is the subscription whose
// refusal decides whether an instrument counts as subscribed. What the flag
// selects is what rides ALONGSIDE it.
struct SubKinds {
    bool trades{true};
    bool depth{true};
    bool funding{true};
};

// Parses "--channels=bbo,funding". Refuses an unknown name rather than
// ignoring it: a typo that silently subscribes everything is a gateway that
// costs what it was asked not to, and says nothing.
//
// `bbo` is accepted and required — naming the channels without it would read
// as a request to drop the quote, which this cannot honour.
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
            return std::unexpected{fmt::format(
                "unknown channel '{}' — bbo, trades, depth or funding", name)};
        }
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    if (!named_bbo) {
        return std::unexpected{std::string{
            "the quote cannot be dropped — name bbo alongside whatever else you want"}};
    }
    return out;
}

// The class decides what a human does next — retry, back off, or page someone —
// so a refusal prints it by name rather than as an integer.
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

// The operator's numbers, turned into the shape exec understands.
//
// exec depends on domain and nothing else, so the translation happens here
// rather than in the library: config/venues.yaml is an operations file, and a
// trading library that parsed one would drag a parser into the order path.
[[nodiscard]] raikiri::exec::RateBudget::Table
budget_table(const raikiri::config::VenueRateSettings& rate) {
    using raikiri::exec::RateBucket;
    const auto to_bucket = [](const raikiri::config::RateBucketSettings& b) {
        return raikiri::exec::RateBudget::BucketConfig{
            .capacity = b.capacity, .window_ns = b.window_ms * 1'000'000ULL, .reserve = b.reserve};
    };
    raikiri::exec::RateBudget::Table table{};
    table[static_cast<std::size_t>(RateBucket::Shared)] = to_bucket(rate.shared);
    table[static_cast<std::size_t>(RateBucket::OrderEntry)] = to_bucket(rate.order_entry);
    table[static_cast<std::size_t>(RateBucket::Account)] = to_bucket(rate.account);
    table[static_cast<std::size_t>(RateBucket::Ledger)] = to_bucket(rate.ledger);
    return table;
}

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

using msgs = raikiri_journal::schema::messages;

// Adapter events -> shm channels + journal. Single-threaded by contract.
class ShmSink final : public raikiri::exec::GatewaySink {
public:
    ShmSink(raikiri::VenueId venue,
            raikiri::shm::GatewayChannels channels,
            raikiri::journal::JournalWriter& journal)
        : venue_{venue}, ch_{channels}, journal_{&journal} {}

    void on_bbo(const raikiri::Bbo& bbo) noexcept override {
        ++counts_.bbo;
        const auto idx = bbo.instrument.symbol().raw;
        if (idx < raikiri::shm::k_bbo_cells) {
            ch_.bbo->write(idx, bbo);
        }
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
    }

    // ONE LADDER PER INSTRUMENT PER WINDOW, and the window is what a reader
    // could possibly resolve. The depth cell holds the latest value; nothing
    // published between two reader passes is ever seen. researchd samples it
    // on a five-second grid and the terminal draws at 30 Hz, so ten
    // milliseconds is two orders finer than the fastest thing that looks.
    //
    // A LATEST-VALUE CELL IS WHY THIS IS SAFE. Dropping a publish loses no
    // information — the next one carries the whole book, including everything
    // the skipped one would have said. That is not true of the trade ring
    // below it, which is a stream, and this must never be copied there.
    // ASKS ONLY. The window is closed by on_depth below, when a ladder is
    // actually written — not here. Closing it on the question would make the
    // pair non-idempotent: an adapter that asked, was told yes, built its
    // snapshot and handed it over would find the second check refusing the
    // publish it had just been granted, and the ladder would vanish.
    [[nodiscard]] bool wants_depth(raikiri::InstrumentId instrument,
                                   raikiri::UnixNanos now) noexcept override {
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
        // ASKED AGAIN HERE, and deliberately. An adapter that called
        // wants_depth() first also saved building the 3.2 KB snapshot; one
        // that did not still has its write refused. Asking twice is safe
        // because the question does not close the window — this does.
        const auto idx = book.instrument.symbol().raw;
        if (!wants_depth(book.instrument, book.ts_init)) {
            return;
        }
        depth_published_ns_[idx] = book.ts_init.ns;
        ++counts_.depth;
        // The frame count says nothing about how deep the book actually is,
        // and a venue that silently ignores a deeper subscription looks
        // identical to one that honoured it.
        counts_.depth_levels = std::max({counts_.depth_levels, book.bid_count, book.ask_count});
        ch_.depth->write(idx, book);
    }

    void on_ticker_stats(const raikiri::TickerStats& stats) noexcept override {
        ++counts_.ticker_stats;
        const auto idx = stats.instrument.symbol().raw;
        if (idx < raikiri::shm::k_ticker_cells) {
            ch_.ticker_stats->write(idx, stats);
        }
        // Not journalled. A 24-hour figure the venue restates every tick is a
        // running total, not an event: replaying it reconstructs nothing, and
        // the only useful reading is the latest one, which the cell holds.
    }

    void on_funding(const raikiri::FundingUpdate& funding) noexcept override {
        ++counts_.funding;
        const auto idx = funding.instrument.symbol().raw;
        if (idx < raikiri::shm::k_funding_cells) {
            ch_.funding->write(idx, funding);
        }
        // ONLY WHEN IT SAYS SOMETHING NEW. A perp position pays or receives at
        // every settlement, so the history of RATES is what a PnL attribution
        // is reconstructed from and it does have to be durable — but a venue
        // restates its rate on a timer whether or not it moved. Measured on
        // okx, three minutes, 477 instruments: 419 936 records written, 971 of
        // them carrying a rate or a settlement time that had changed. The other
        // 99.77% filled 256 MiB every three minutes — 120 GB a day from one
        // venue, on a gateway that had never placed an order.
        //
        // The latest value is in the shm cell for anyone who wants it. What has
        // to survive a restart is the CHANGE.
        if (idx < raikiri::shm::k_funding_cells) {
            const Stated now{.rate = funding.rate.raw,
                             .next = funding.next_funding_time.ns,
                             .interval_s = funding.interval_s};
            if (stated_[idx] == now) {
                return;
            }
            stated_[idx] = now;
        }
        (void)raikiri::schema::append_msg<msgs::FundingMsg>(
            *journal_, funding.ts_init.ns, [&](auto m) {
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

    // A refusal the venue never saw. Synthesized locally so the order reaches a
    // terminal state instead of waiting for a verdict that will never come —
    // the same shape as a REST-level rejection, which venues also never push.
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
        on_order_error(
            cmd.client_order_id,
            raikiri::exec::VenueError::make(raikiri::exec::ErrorClass::Terminal, 0, why));
        on_order_event(event);
    }

    // The only place a refusal becomes readable. Nothing else in the process
    // prints one: `OrderEvent` reaches shm and the journal carrying the fact of
    // a rejection and no reason for it, so an order that dies at the venue is
    // invisible here unless this line runs.
    void on_order_error(raikiri::ClientOrderId client_order_id,
                        const raikiri::exec::VenueError& error) noexcept override {
        ++counts_.order_errors;
        // Held for the rejection event that follows it; see there.
        pending_ = {client_order_id.raw, error};
        RLOG_WARN("gatewayd: refused coid={:#x} — {} (class={}, code={})",
                  client_order_id.raw,
                  error.message.view(),
                  error_class_name(error.cls),
                  error.venue_code);
    }

    // COUNTED HERE, PUBLISHED BY THE POLLER. This runs on the gateway thread
    // and `publish_funding_charges` runs on the account poller's, and
    // BroadcastRing is single-producer (broadcast_ring.hpp): two threads
    // picking the same sequence stamp one slot's version twice, interleave
    // their word stores, and a reader re-validates a torn record as good — a
    // settlement the ledger books at an amount neither leg reported. `published`
    // can also move backwards, which resets a reader's cursor with no gap.
    //
    // The poller is the leg that keeps it, and losing this one costs only
    // latency: it re-publishes the whole window on a timer (see there for why),
    // so a charge the private socket saw first is booked on the next pass
    // rather than never. Funding settles a few times a day.
    //
    // The count stays, because it is the only place that says the private
    // socket is delivering charges at all.
    void on_funding_charge(const raikiri::LedgerEntry&) noexcept override {
        ++counts_.funding_charges;
    }

    // The three kinds that carry a refusal. Nothing else does, and stamping a
    // reason onto a fill would put a code on money that moved.
    [[nodiscard]] static bool is_rejection(raikiri::OrderEventType type) noexcept {
        return type == raikiri::OrderEventType::Rejected ||
               type == raikiri::OrderEventType::CancelRejected ||
               type == raikiri::OrderEventType::ModifyRejected;
    }

    void on_order_event(const raikiri::OrderEvent& raw) noexcept override {
        ++counts_.order_events;
        // WHY THE VENUE SAID NO, JOINED HERE RATHER THAN IN SEVENTEEN ADAPTERS.
        //
        // Every adapter reports a refusal as a PAIR — on_order_error with the
        // venue's verdict, then immediately on_order_event with the rejection —
        // emitted back to back on this one thread with nothing between them.
        // That adjacency is the contract this join rests on, and it is checked
        // by a test rather than assumed.
        //
        // One slot, not a map: the pair is adjacent, so anything older than the
        // last error is not the answer to this event and must not be stamped
        // onto it. An id that does not match leaves the event as it came.
        auto event = raw;
        if (is_rejection(event.type) && pending_.first == event.client_order_id.raw) {
            event.reject_class = pending_.second.cls;
            event.reject_code = static_cast<std::int32_t>(pending_.second.venue_code);
            pending_ = {};
        }
        ch_.exec_events->publish({event});
        (void)raikiri::schema::append_msg<msgs::OrderEventMsg>(
            *journal_, event.ts_init.ns, [&](auto m) {
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
        // Ladders the adapter would have built and published, refused inside
        // the window. Beside `depth` on purpose: the pair is the whole
        // argument for the gate, and a ratio near one would mean it is not
        // earning its complexity on this venue.
        std::uint64_t depth_coalesced{0};
        std::uint32_t depth_levels{0};
        std::uint64_t funding{0};
        std::uint64_t ticker_stats{0};
        std::uint64_t greeks{0};
        std::uint64_t order_events{0};
        std::uint64_t order_errors{0};
        std::uint64_t funding_charges{0};
    };

    // The last refusal and whose order it was, waiting for the rejection event
    // that follows it. Zeroed once claimed.
    std::pair<std::uint64_t, raikiri::exec::VenueError> pending_{};

    [[nodiscard]] const Counts& counts() const noexcept { return counts_; }

    void on_connectivity(const raikiri::exec::ConnEvent& event) noexcept override {
        (void)raikiri::schema::append_msg<msgs::ConnectivityMsg>(
            *journal_, event.ts.ns, [&](auto m) {
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

    // When each instrument's ladder was last published. Wall time, because
    // that is what the adapters hand in; a venue that stamps the future would
    // suppress its own next publish, which is bounded at one window.
    std::array<std::uint64_t, raikiri::shm::k_depth_cells> depth_published_ns_{};

    // The last rate journalled per instrument, so a restatement of the same one
    // is not written again. Deliberately NOT the shm cell: that holds the
    // latest published value, which is updated whether or not this wrote it.
    struct Stated {
        std::int64_t rate{raikiri::FixedE9{}.raw};
        std::uint64_t next{0};
        std::uint32_t interval_s{0};

        [[nodiscard]] bool operator==(const Stated&) const noexcept = default;
    };
    std::array<Stated, raikiri::shm::k_funding_cells> stated_{};
    std::uint64_t md_seq_{0};
    Counts counts_{};
};

} // namespace

// A pull every 30s, and once at startup. Not a hot path: this is the slow,
// authoritative check that the fast path is still telling the truth.
// How often the account poller asks the venue what it holds. Thirty seconds,
// as before — what changed is which thread waits for the answer.
constexpr int k_recon_interval_slices = 300; // 300 x 100 ms

// Publishes what the venue says it holds, and nothing else.
//
// The comparison used to live here, against a quantity this process netted
// from its own event stream. That number was derived by a different rule than
// the one strategyd uses for the same events, so two processes disagreed about
// what "we" hold and the venue was compared against the wrong one. The book
// has exactly one owner; this gateway is not it. So the pull is published as
// state and the verdict is reached where the book is.
//
// A SUCCESSFUL pull writes every catalogued instrument, zeros included: that
// is what makes a fresh zero mean "the venue says flat" rather than "nobody
// asked". A pull that fails writes nothing at all, leaving the cells stale,
// which is how the reader reaches Unknown without a flag saying so.
// The account poller's sink, which accepts nothing.
//
// The poller's adapter is REST-only and never starts a socket, so in principle
// it has nothing to push. This makes that a property of the code rather than
// of anyone's reading of it: if a future adapter did emit from a pull path, it
// would land here and be dropped, not double-counted into the gateway's real
// event stream from a second thread.
struct NullSink final : raikiri::exec::GatewaySink {
    void on_bbo(const raikiri::Bbo&) noexcept override {}

    void on_trade(const raikiri::TradeTick&) noexcept override {}

    void on_ticker_stats(const raikiri::TickerStats&) noexcept override {}

    void on_funding(const raikiri::FundingUpdate&) noexcept override {}

    void on_order_event(const raikiri::OrderEvent&) noexcept override {}

    void on_connectivity(const raikiri::exec::ConnEvent&) noexcept override {}
};

// What the venue charged, onto the ring the book reads.
//
// A CURSOR, NOT A WINDOW. The pull asks for everything since the last charge
// already seen, so a settlement that lands between two passes is picked up on
// the next one rather than missed. The cursor only ever moves forward, and a
// failed pull does not move it — a charge is money, and skipping one because a
// request timed out would be a silent loss.
// What the venue still has working, onto the table the book compares against.
//
// WRITTEN WHOLESALE OR NOT AT ALL. A successful pull writes every order the
// venue named and clears the rest, so absence is the venue's word; a failed
// pull writes nothing, so every cell ages together and a reader sees silence
// rather than an empty book. Closing a desk's orders because a request timed
// out is the failure this shape exists to make impossible.
void publish_open_orders(const raikiri::exec::VenueAdapter& adapter,
                         raikiri::MarketType market,
                         raikiri::shm::OpenOrderTable& table,
                         raikiri::UnixNanos now) {
    char buf[16];
    const auto who =
        raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
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
        // Refused rather than truncated: a partial list read as complete would
        // close every order past the cut.
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
    // The tail is CLEARED, not left: a cell still holding last pass's order
    // would keep an order alive in the reader's eyes after the venue dropped it.
    for (; slot < raikiri::shm::k_open_order_cells; ++slot) {
        table.write(slot, raikiri::shm::OpenOrderCell{});
    }
    // SAID AGAIN WHEN IT CHANGES, and not once per process. Said once, this
    // line reports the count at boot for the life of the run — a gateway that
    // printed "0 working" before anybody placed an order goes on saying it
    // while orders rest at the venue, and an operator reading the log has a
    // number that was true for one second an hour ago. An unchanged count
    // stays quiet, which is what the once-only flag was really protecting.
    static std::size_t said = std::numeric_limits<std::size_t>::max();
    if (said != reports->size()) {
        said = reports->size();
        RLOG_INFO(
            "gatewayd[{}]: venue answered its open orders — {} working", who, reports->size());
    }
}

// HOW FAR BACK A FILL PULL RESUMES from the newest it saw. A venue may hold a
// trade back for a moment after the one beside it, and resuming at the newest
// stamp would step over it forever. A minute of overlap costs a few repeated
// rows that the reader admits once by id.
constexpr std::uint64_t k_fill_overlap_ns = 60ULL * 1'000'000'000ULL;

// How often the whole window is written again for whoever has since attached.
// Ten minutes: often enough that a strategyd restart loses nothing for long,
// rare enough that it is not a second feed.
constexpr std::uint64_t k_ledger_replay_interval_ns = 600ULL * 1'000'000'000ULL;

void publish_funding_charges(const raikiri::exec::VenueAdapter& adapter,
                             raikiri::MarketType market,
                             raikiri::shm::LedgerEntryRing& ring,
                             raikiri::UnixNanos& cursor,
                             raikiri::UnixNanos& replay_due,
                             raikiri::UnixNanos now) {
    char buf[16];
    const auto who =
        raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
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
    // RE-PUBLISHED PERIODICALLY, not only when new. The ring is a broadcast:
    // a reader that attaches after a charge was written never sees it, and the
    // cursor below would make sure it never sees it again either — money the
    // ledger silently never books. Replaying the whole window costs nothing
    // downstream, because the ledger keys on the settlement itself and drops a
    // repeat.
    const bool replay = now.ns >= replay_due.ns;
    if (replay) {
        replay_due = raikiri::UnixNanos{now.ns + k_ledger_replay_interval_ns};
    }
    for (const auto& charge : *charges) {
        if (!replay && charge.settled_at.ns <= cursor.ns) {
            continue; // already published on an earlier pass
        }
        ring.publish({charge});
        // Money moving is worth a line. Funding settles a few times a day per
        // instrument, so this is a record an operator can read, not a stream.
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

// What the venue says was EXECUTED, for the one route a fill has when no
// private socket carries it — and a net under the socket where one does.
//
// SINGLE WRITER, like the charges beside it. `exec_events` is the gateway
// thread's ring and this runs on the poller's; two producers on a BroadcastRing
// stamp one slot's version twice and a reader re-validates a torn record as
// good. So a pulled fill rides its own ring and the consumer joins the two.
//
// NO CURSOR ADVANCE ON FAILURE, and the window is re-asked rather than
// narrowed: the venue re-offers what it already gave, the reader admits each
// execution once by its id, and a pull that failed must not leave a hole no
// later pass reaches back over.
// NO `now`: the cursor moves off the venue's own stamps, never this clock. A
// local clock ahead of the venue's would step the window past trades it had not
// finished reporting.
void publish_fills(const raikiri::exec::VenueAdapter& adapter,
                   raikiri::MarketType market,
                   raikiri::shm::FillRing& ring,
                   raikiri::UnixNanos& cursor) {
    char buf[16];
    const auto who =
        raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
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
    // THE OLDEST STAMP IS NOT WHERE TO RESUME. A venue answers a window and may
    // hold a trade back for a moment; resuming at the newest would skip it.
    // Backing off keeps the overlap the reader already dedupes.
    for (const auto& fill : *fills) {
        if (fill.ts_venue.ns > cursor.ns + k_fill_overlap_ns) {
            cursor = raikiri::UnixNanos{fill.ts_venue.ns - k_fill_overlap_ns};
        }
    }
    static std::size_t said = std::numeric_limits<std::size_t>::max();
    if (said != fills->size()) {
        said = fills->size();
        RLOG_INFO("gatewayd[{}]: venue answered its trade history — {} fill(s) in the window",
                  who,
                  fills->size());
    }
}

void publish_positions(const raikiri::exec::VenueAdapter& adapter,
                       raikiri::MarketType market,
                       const raikiri::shm::CatalogueTable& catalogue,
                       raikiri::shm::PositionTable& table,
                       raikiri::UnixNanos now) {
    char buf[16];
    const auto who =
        raikiri::format_account({.venue = adapter.caps.venue, .market = market}, buf, sizeof buf);
    static bool announced = false;
    static std::size_t answered = std::numeric_limits<std::size_t>::max();
    if (!adapter.caps.position_pull.has(market)) {
        if (!announced) {
            // Said once, at start, and never again: a venue that cannot answer
            // is a standing condition, not an event, and repeating it every
            // 30s would train an operator to ignore the line that matters.
            announced = true;
            RLOG_INFO("gatewayd[{}]: position reconciliation unavailable on this market", who);
        }
        return;
    }

    const auto reports = adapter.execution->position_reports(market);
    if (!reports.has_value()) {
        // Not a break and not flatness — the venue declined to answer this
        // round. Publishing nothing is the whole point: the cells age out and
        // the reader sees Unknown.
        RLOG_WARN("gatewayd[{}]: position pull failed — {}", who, reports.error().message.view());
        return;
    }

    // The other half of the line above. Without it an authenticated venue
    // holding nothing looks exactly like a venue that was never asked, which is
    // the distinction this whole channel exists to keep.
    // Said again when it changes, for the reason written over the open-order
    // count: once-only makes the line describe boot forever.
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
            // The venue answered and did not name this instrument, which on a
            // complete snapshot means flat. Written out field by field because
            // a default-constructed QtyLots is `undef`, not zero: leaving it
            // alone would publish INT64_MIN as a position.
            cell.report.instrument = id;
            cell.report.net_qty = raikiri::QtyLots::from_raw(0);
            cell.report.avg_entry = raikiri::PriceTicks::from_raw(0);
            cell.report.ts_venue = now;
        }
        cell.ts_pulled = now;
        table.write(idx, cell);
    }
}

// What the account holds, published as ONE cell.
//
// Same rule as the positions beside it: a failed pull writes nothing and the
// snapshot ages out, so a venue that could not answer never decays into an
// account that holds nothing. The difference is that this is a set — a reader
// that saw half of it would compute an equity the account never had — so the
// whole thing is one seqlock write rather than a row per asset.
void publish_balances(const raikiri::exec::VenueAdapter& adapter,
                      raikiri::MarketType market,
                      raikiri::shm::BalanceTable& table,
                      raikiri::UnixNanos now) {
    std::array<char, 16> buf{};
    const auto who = raikiri::format_account(
        {.venue = adapter.caps.venue, .market = market}, buf.data(), buf.size());
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
        // Refused rather than truncated: a partial balance set sums to an
        // equity nobody holds, and this cell exists so a reader never sees one.
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

    // Said again when it changes, for the reason written over the open-order
    // count: once-only makes the line describe boot forever.
    if (answered != snapshot.count) {
        answered = snapshot.count;
        RLOG_INFO("gatewayd[{}]: venue answered its balances — {} asset(s)", who, snapshot.count);
    }
}

// What the venue said about our budget, kept as the LAST value seen per header
// so a run ends able to state the venue's own numbers.
//
// Guarded, because the account poller runs its REST calls on its own thread
// while the gateway loop runs the rest.
class RateWitness {
public:
    void
    observe(std::string_view host, const raikiri::net::RateHeader& header, raikiri::UnixNanos at) {
        const std::lock_guard lock{mu_};
        seen_[std::string{host} + " " + header.name] = Seen{.value = header.value, .at = at};
    }

    void report(std::string_view account) const {
        const std::lock_guard lock{mu_};
        if (seen_.empty()) {
            return; // a venue that states nothing must not be reported as zero
        }
        for (const auto& [key, seen] : seen_) {
            // WITH THE INSTANT IT WAS READ AT. Half these venues state their
            // window as a reset stamp or a millisecond countdown, and a
            // countdown without the moment it was counted from establishes no
            // window at all — which is how a budget gets read off a
            // documentation page instead.
            RLOG_INFO("gatewayd[{}]: venue's own metering — {} = {} (read at {})",
                      account,
                      key,
                      seen.value,
                      seen.at.ns);
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

// WHAT THIS PROCESS SPENT ITS ALLOWANCE ON.
//
// A venue's ban message names no caller. The market-data pools are not metered
// by RateBudget — only the order path is — so their traffic was invisible to
// every counter in the process, and a repeated catalogue fetch or a 24-hour
// ticker poll could earn an IP ban with nothing to point at afterwards.
//
// The tally is always kept and printed at shutdown, because it is small and it
// is the thing that answers the question. RAIKIRI_LOG_REQUESTS=1 additionally
// prints every request as it completes, which is far too loud to leave on.
class RequestLedger {
public:
    // A segment naming an INSTRUMENT is collapsed; a segment naming the API is
    // not. Venue symbols here are upper case — XBTUSDTM, BTC-USDT-SWAP — and
    // no endpoint name is, while an api version is `v1` and must survive.
    // Getting this backwards is easy and was: keying on "contains a digit"
    // collapsed the version and kept every symbol, which is the 313-row tally
    // this exists to prevent.
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
        // Keyed on the SHAPE of the path, not the path. A query string carries
        // symbols and cursors — and some venues put the symbol in the path
        // itself, which turned this tally into 313 rows of
        // `/funding-rate/<symbol>/current` and buried the endpoint that
        // actually cost the allowance. A segment that names an instrument is
        // collapsed so the endpoint is what gets counted.
        const auto path = collapse(target.substr(0, target.find('?')));
        const std::lock_guard lock{mu_};
        auto& row = spent_[std::string{host}.append(path)];
        ++row.calls;
        if (status < 200 || status >= 300) {
            ++row.refused;
            row.last_status = status;
        }
        if (verbose_) {
            RLOG_INFO("gatewayd[{}]: {} {} -> {} in {} ms",
                      account,
                      host,
                      target,
                      status,
                      elapsed_ns / 1'000'000);
        }
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
    bool verbose_;
};

int main(int argc, char** argv) {
    // NOTHING HERE WRITES TO STDOUT. Every line this process emits goes to
    // stderr, which is unbuffered; a print added to stdout would need the line
    // buffering set back, or a redirected gateway shows an empty log while it
    // runs and everything at once when it dies.
    //
    // Named for the process, not the account: the account is not parsed yet,
    // and it is already the first thing in every message this gateway writes.
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
    // EVERY CHANNEL BY DEFAULT, so an existing command line means what it
    // always meant.
    //
    // IT DOES NOT SAVE CPU, and the number was measured rather than assumed:
    // okx, 471 instruments, two minutes each — 43.8% of a core with every
    // channel and 43.0% with depth and trades off, though depth really did go
    // from 138 931 frames to none. A BookSnapshot is large to WRITE (a hundred
    // levels a side, ~3200 bytes against a Bbo's 48) and that write is a
    // memcpy; the processor time goes on receiving, decrypting and parsing,
    // which scales with the NUMBER of messages a socket delivers, and that
    // barely moved.
    //
    // What it is for: a gateway run only to widen a scanner's venue coverage
    // publishes no ladder nobody reads — less churn in the segment, and fewer
    // cells for every reader's change gate to walk. To spend less CPU, run
    // fewer venues; the cost is about the venue, not about the channel.
    SubKinds wanted_channels{}; // all on
    std::string channels_arg;
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
        } else if (arg.starts_with("--symbol=")) {
            symbols.emplace_back(arg.substr(9));
        } else if (arg.starts_with("--api-key=")) {
            // A REFERENCE, never a key: "env:NAME", "file:/path", "exec:cmd".
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
        // Which venues exist is the registry's answer, not a list maintained
        // here: make_venue() reports UnknownVenue for anything unregistered,
        // so adding an adapter never means editing this function.
        fmt::print(stderr, "gatewayd: unknown --venue={}\n", venue_name);
        return 2;
    }
    // Credentials are optional on purpose: public market data needs none, so
    // a gateway can run read-only against a live venue with no keys present
    // anywhere on the box.
    if (!api_key_ref.empty()) {
        config::disable_core_dumps();
    }
    // Report WHERE each credential comes from, never what it is. Parsing the
    // reference is enough for that — the value is resolved inside the adapter
    // and never reaches this function.
    const auto report_credential = [](std::string_view what, const std::string& spec) {
        if (spec.empty()) {
            return true;
        }
        const auto ref = config::SecretRef::parse(spec);
        if (!ref.has_value()) {
            RLOG_ERROR(
                "gatewayd: {} reference is malformed ({})", what, config::to_string(ref.error()));
            return false;
        }
        RLOG_INFO("gatewayd: {} from {}", what, config::to_string(ref->scheme));
        if (ref->scheme == config::SecretRef::Scheme::OnePassword) {
            // Reading the secret would say whether it worked, but only after
            // the adapter has already decided to run read-only. The probe
            // touches no secret and answers the question before that.
            const auto status = config::probe_one_password();
            if (!status.usable()) {
                RLOG_ERROR("gatewayd: 1password unusable — {}", status.diagnostic);
                RLOG_ERROR("gatewayd: on a server set OP_SERVICE_ACCOUNT_TOKEN; on a laptop "
                           "run `eval $(op signin)`");
                return false;
            }
            RLOG_INFO(
                "gatewayd: 1password {} as {}", config::to_string(status.auth), status.account);
        }
        if (config::is_plaintext(ref->scheme)) {
            // Deliberately available and deliberately loud: it is the one
            // scheme that puts key material into a file that gets committed,
            // baked into an image, and pasted into a bug report.
            RLOG_WARN("gatewayd: {} is a literal value in configuration. Fine for a demo; "
                      "for anything holding real funds use env:, file: or exec: (e.g. "
                      "exec:op read op://vault/item/field).",
                      what);
        }
        return true;
    };
    if (!report_credential("api key", api_key_ref) ||
        !report_credential("api secret", api_secret_ref) ||
        !report_credential("passphrase", passphrase_ref)) {
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;

    // Which market this gateway serves. A venue whose adapter does not serve
    // it cannot be asked for it, and asking used to fail as "adapter build
    // failed" with nothing naming the market.
    const auto& caps = exec::caps_of(venue);
    MarketType market = MarketType::Perp;
    if (market_arg == "spot") {
        market = MarketType::Spot;
    } else if (market_arg == "future") {
        market = MarketType::Future;
    } else if (market_arg == "option") {
        market = MarketType::Option;
    } else if (market_arg == "perp" || market_arg.empty()) {
        // Defaulting to spot when a venue lists no perpetuals keeps
        // `--venue=coinbase` working without a flag; it is a default, not a
        // guess, because the caps row is the thing being read.
        market = caps.markets.perp ? MarketType::Perp : MarketType::Spot;
    } else {
        fmt::print(stderr, "gatewayd: --market must be spot, perp, future or option\n");
        return 2;
    }
    if (!caps.serves(market)) {
        RLOG_ERROR("gatewayd[{}]: this adapter does not serve the {} market",
                   venue_name,
                   to_string(market));
        return 2;
    }

    // This process serves exactly one market, and the account name is how
    // every path it owns says so — segment, journal, and the log prefix.
    const AccountId account{.venue = venue, .market = market};
    char account_buf[16];
    const auto account_name = std::string{format_account(account, account_buf, sizeof account_buf)};

    // shm segment (owner side).
    const auto seg_path = shm::gateway_segment_path(account);
    if (!journal::detail::mkdir_p(platform::runtime_dir() + "/shm")) {
        RLOG_ERROR("gatewayd: cannot create runtime dir");
        return 1;
    }
    auto owner = shm::SegmentOwner::create(
        seg_path, shm::k_gateway_segment_bytes, clock.mono().ns, clock.mono().ns);
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
    // WHAT THIS DEPLOYMENT CAN AFFORD TO DROP. The library keeps everything by
    // default because it cannot see how far a reader has drained; this process
    // can at least bound the disk. Eight segments is two gibibytes an account —
    // and a persister further behind than that loses what it had not read, so a
    // box that keeps its history must ship a sealed segment off it faster than
    // eight fill. RAIKIRI_JOURNAL_RETAIN=0 keeps the old unbounded behaviour.
    constexpr std::size_t k_retain_segments = 8;
    auto retain = k_retain_segments;
    if (const char* env = std::getenv("RAIKIRI_JOURNAL_RETAIN"); env != nullptr && *env != '\0') {
        retain = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
    }
    auto journal_writer =
        journal::JournalWriter::open(data_dir + "/journal/gatewayd-" + account_name,
                                     "journal",
                                     journal::k_default_segment_bytes,
                                     retain);
    if (!journal_writer.has_value()) {
        // AlreadyWriting is the interesting one: another gatewayd for this
        // same account is live, and the segment did not catch it because that
        // process may own a different path under an older naming.
        RLOG_ERROR("gatewayd[{}]: journal open failed{}",
                   account_name,
                   journal_writer.error() == journal::JournalError::AlreadyWriting
                       ? " — another process is already writing it"
                       : "");
        return 1;
    }

    ShmSink sink{venue, *channels, *journal_writer};

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
    // ONE BUDGET FOR THE PROCESS, shared by the order path and the account
    // poller below, because the venue counts them together against one IP.
    // A venue absent from the file, or one whose buckets are all zero, is
    // UNLIMITED here — that is the honest reading of a budget nobody has
    // measured, and inventing one would refuse traffic the venue accepts.
    std::optional<exec::RateBudget> budget;
    // THE TWO SETTINGS PART COMPANY WHEN THE VENUE IS ABSENT. A rate budget
    // nobody measured must stay unset — inventing one refuses traffic the
    // venue would have taken. The answer timeout is the opposite: zero means
    // an unanswered order is never reported at all, and thirteen of sixteen
    // venues are not in that file, so an absent venue took the setting the
    // struct already documents as the right default rather than none.
    std::uint64_t answer_timeout_ns =
        config::VenueSettings{}.order_answer_timeout_ms * 1'000'000ULL;
    std::uint32_t book_depth = 0; // the adapter's own tier until a file says otherwise
    std::string market_data_feed; // and the adapter's own feed
    const auto venues_path = config::config_dir() + "/venues.yaml";
    if (const auto settings = config::load_venues(venues_path); settings.has_value()) {
        if (const auto it = settings->venues.find(std::string{venue_name});
            it != settings->venues.end()) {
            const auto& rate = it->second.rate;
            budget.emplace(budget_table(rate),
                           exec::RateBudget::Costs{.order = rate.costs.order,
                                                   .positions = rate.costs.positions,
                                                   .balances = rate.costs.balances,
                                                   .ledger = rate.costs.ledger});
            answer_timeout_ns = it->second.order_answer_timeout_ms * 1'000'000ULL;
            book_depth = it->second.book_depth;
            market_data_feed = it->second.market_data_feed;
        } else {
            RLOG_INFO("gatewayd[{}]: no rate budget configured — running unlimited", account_name);
        }
    } else {
        // Said out loud rather than swallowed: running unlimited because a file
        // failed to parse is a different fact from running unlimited on purpose.
        RLOG_WARN("gatewayd: {} — running unlimited", settings.error());
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
    if (const auto started = adapter->data->start(); !started.has_value()) {
        RLOG_ERROR("gatewayd: market data failed to start — {}", started.error().message.view());
        return 1;
    }
    // Order entry is allowed to refuse when no key was configured, and that is
    // NOT fatal: the whole repository is meant to clone and run against live
    // venues with no credentials at all, and tools/venue_smoke.sh does exactly
    // that. What would be fatal is a gateway that accepted orders it could not
    // sign, so the command rings are only drained once execution is up.
    const bool credentials_configured = !api_key_ref.empty() && !api_secret_ref.empty();
    bool can_trade = false;
    if (const auto started = adapter->execution->start(); started.has_value()) {
        can_trade = true;
    } else if (credentials_configured) {
        RLOG_ERROR("gatewayd: order entry failed to start — {}", started.error().message.view());
        return 1;
    } else {
        RLOG_INFO(
            "gatewayd[{}]: market data only — {}", account_name, started.error().message.view());
    }
    // The catalogue must load before any subscription: an adapter cannot map
    // a single inbound frame without the tick and lot sizes, and a gateway
    // that subscribed first would silently drop everything until the table
    // filled.
    std::vector<Instrument> catalogue;
    if (venue == VenueId::Sim) {
        catalogue = demo::sim_instruments();
    } else {
        auto loaded = adapter->instruments->fetch_all(market);
        if (!loaded.has_value()) {
            RLOG_ERROR("gatewayd[{}]: catalogue load failed: {} (code {})",
                       account_name,
                       loaded.error().message.view(),
                       loaded.error().venue_code);
            return 1;
        }
        catalogue = std::move(*loaded);
    }
    RLOG_INFO("gatewayd[{}]: {} instruments loaded", account_name, catalogue.size());

    // Publish the numbering before the segment goes Ready, so a consumer that
    // can attach at all can resolve every id the gateway will ever send.
    for (const auto& instrument : catalogue) {
        if (const auto pushed = channels->catalogue->push(instrument); !pushed.has_value()) {
            RLOG_ERROR("gatewayd[{}]: catalogue publish refused symbol {}",
                       account_name,
                       base_of(instrument).exchange_symbol.view());
            return 1;
        }
    }

    std::size_t matched = 0;
    std::size_t subscribed = 0;
    std::string first_refusal;
    for (const auto& instrument : catalogue) {
        const auto& base = base_of(instrument);
        const auto wanted = symbols.empty() ||
                            std::any_of(symbols.begin(), symbols.end(), [&](const std::string& s) {
                                return s == base.exchange_symbol.view();
                            });
        if (!wanted) {
            continue;
        }
        ++matched;
        // The BBO result is the one that decides. Trades, depth and funding are
        // allowed to be missing on a venue that publishes none of them, but an
        // adapter with no top-of-book has no market data at all — and this
        // count used to be of instruments CONSIDERED, so a gateway whose every
        // subscription was refused still announced that it had subscribed.
        const auto bbo = adapter->data->subscribe(exec::SubKind::Bbo, base.id);
        if (!bbo.has_value()) {
            if (first_refusal.empty()) {
                first_refusal = std::string{bbo.error().message.view()};
            }
            continue;
        }
        ++subscribed;
        if (wanted_channels.trades) {
            (void)adapter->data->subscribe(exec::SubKind::Trades, base.id);
        }
        if (wanted_channels.depth) {
            (void)adapter->data->subscribe(exec::SubKind::Depth, base.id);
        }
        if (wanted_channels.funding && venue != VenueId::Sim) {
            (void)adapter->data->subscribe(exec::SubKind::Funding, base.id);
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
    // SAID OUT LOUD, not swallowed. `first_refusal` used to be collected and
    // then discarded unless EVERY subscription failed, so a gateway that got
    // 256 of 979 announced the 256 and nothing else — and the other 723
    // instruments sat on the markets board with every column empty, which
    // reads as a dead venue rather than an unsubscribed instrument.
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
        [&requests, &account_name](
            std::string_view host, std::string_view target, int status, std::uint64_t elapsed_ns) {
            requests.observe(host, target, status, elapsed_ns, account_name);
        });
    RateWitness rates;
    raikiri::net::set_rate_observer(
        [&rates, &clock](std::string_view host, const raikiri::net::RateHeader& header) {
            rates.observe(host, header, clock.wall());
        });

    RLOG_INFO("gatewayd[{}]: up, segment {}", account_name, seg_path);

    shm::SpscConsumer creates{channels->cmd_create};
    shm::SpscConsumer cancels{channels->cmd_cancel};

    // THE ACCOUNT POLLER — the venue's word on what this account holds, asked
    // for on its own thread.
    //
    // It gets its OWN adapter rather than sharing the loop's. Every venue
    // adapter documents its state as single-threaded ("the gateway loop is the
    // only caller, which is why nothing here synchronises"), and a shared one
    // would need a lock the loop would then wait on for the length of an HTTP
    // round trip — the same stall, moved. A second adapter is a second signer
    // and a second connection pool, and that is the price of the isolation.
    //
    // `data->start()` is NOT called: a second market-data socket would be pure
    // waste. `execution->start()` IS, because it does three things and the
    // pulls need two of them — it brings up the REST client and, critically,
    // syncs this client's clock against the venue. Signing with an unsynced
    // clock gets every request refused with "check your server timestamp",
    // which is exactly what happened when this was left out.
    //
    // Its third act is opening a user socket, and that is why the poller has a
    // sink that accepts nothing: the duplicate fills it would deliver are
    // dropped at the boundary rather than counted twice. The cost is one extra
    // private connection per gateway, which is the price of not threading a
    // pull-only start through sixteen adapters.
    //
    // It is also the ONLY writer of these two tables, which keeps the
    // single-writer contract the seqlock rests on.
    std::unique_ptr<exec::VenueAdapter> poller_adapter;
    NullSink poller_sink;
    std::thread poller;
    // Beside the thread that reads it, not inside the block that starts it: the
    // poller outlives that block, and a cursor scoped to it is a reference the
    // thread keeps using after the stack frame is gone.
    //
    // Seeded at zero so the first pass asks for the whole window the venue
    // allows — a gateway that restarted mid-interval must not skip the
    // settlement it was down for.
    raikiri::UnixNanos funding_cursor{};
    raikiri::UnixNanos ledger_replay_due{};
    // Where the trade-history pull resumes. Starts at zero, which asks the
    // venue for its whole default window once — a gateway that restarts after
    // a fill must not miss it. OUT HERE for the reason stated above: the
    // poller thread captures it by reference and outlives the block below.
    raikiri::UnixNanos fill_cursor{};
    if (can_trade) {
        auto made = exec::make_venue({.venue = venue,
                                      .market = market,
                                      .testnet = testnet,
                                      .api_key_env = api_key_ref,
                                      .api_secret_env = api_secret_ref,
                                      .passphrase_env = passphrase_ref,
                                      // THE SAME budget object, not a copy.
                                      .budget = budget.has_value() ? &*budget : nullptr,
                                      // The poller sends no orders, so nothing
                                      // of its own ever waits for an answer.
                                      .order_answer_timeout_ns = 0,
                                      // NAMED EVEN THOUGH THIS ADAPTER READS NO
                                      // MARKET DATA: gcc treats a designated
                                      // initializer that skips a trailing
                                      // member as -Wmissing-field-initializers,
                                      // which is an error here, and clang does
                                      // not — so a field added for one call
                                      // site breaks the build on the server and
                                      // nowhere else.
                                      .book_depth = 0,
                                      .market_data_feed = {}},
                                     poller_sink,
                                     clock);
        if (made.has_value()) {
            poller_adapter = std::make_unique<exec::VenueAdapter>(std::move(*made));
            if (const auto up = poller_adapter->execution->start(); !up.has_value()) {
                RLOG_WARN("gatewayd: account poller could not start — {}",
                          up.error().message.view());
                poller_adapter.reset();
            }
            // ITS OWN CATALOGUE, because it has its own instrument table.
            // `fetch_all` interns into the adapter it is called on, and this is
            // a second adapter: without this call the table stays empty, every
            // position the venue reports maps to nothing, and the poller says
            // the account is flat while it holds a position. Balances hid it —
            // they key on an asset name and need no instrument at all.
            if (poller_adapter != nullptr) {
                if (const auto loaded = poller_adapter->instruments->fetch_all(market);
                    !loaded.has_value()) {
                    RLOG_WARN("gatewayd: account poller has no catalogue — {}",
                              loaded.error().message.view());
                    poller_adapter.reset();
                }
            }
        }
        if (poller_adapter != nullptr) {
            poller = std::thread([&] {
                auto* channels_ptr = &*channels;
                while (!g_stop.load(std::memory_order_relaxed)) {
                    publish_positions(*poller_adapter,
                                      market,
                                      *channels_ptr->catalogue,
                                      *channels_ptr->positions,
                                      clock.wall());
                    // On the same pass as the positions: both are the venue's
                    // word on this account, and asking for one without the
                    // other invites a reader to compare numbers taken thirty
                    // seconds apart.
                    publish_balances(
                        *poller_adapter, market, *channels_ptr->balances, clock.wall());
                    // On the same pass as the positions: both are the venue's
                    // word on this account.
                    publish_open_orders(
                        *poller_adapter, market, *channels_ptr->open_orders, clock.wall());
                    publish_funding_charges(*poller_adapter,
                                            market,
                                            *channels_ptr->funding_charges,
                                            funding_cursor,
                                            ledger_replay_due,
                                            clock.wall());
                    // On the same pass, and for the same reason: what the
                    // account was charged and what it executed are one answer
                    // about one account.
                    publish_fills(
                        *poller_adapter, market, *channels_ptr->fills, fill_cursor);
                    // Woken in slices so a shutdown does not wait out the
                    // whole interval.
                    for (int i = 0;
                         i < k_recon_interval_slices && !g_stop.load(std::memory_order_relaxed);
                         ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{100});
                    }
                }
            });
        }
    }

    const auto t_end =
        duration_s != 0 ? clock.mono() + duration_s * 1'000'000'000ULL : MonoNanos{~0ULL};
    std::uint64_t flush_due = 0;
    // SPIN WHILE THERE IS TRADING, PARK WHEN THERE IS NOT.
    //
    // This loop used to spin unconditionally, and the cost of that was the
    // whole cost: measured on okx, one symbol burned 35.5% of a core and the
    // entire 471-instrument catalogue — ninety-four times the messages —
    // burned 37.8%. Nearly all of it was the loop going round, not the data
    // going through, which is also why subscribing fewer channels saved
    // nothing.
    //
    // The park is only ever reached when the command rings came up empty
    // several passes running, so an order or a cancel is still drained at spin
    // speed for as long as they keep arriving. What it costs is up to
    // k_park_ns of extra latency on the FIRST command after a quiet spell,
    // against a venue round trip measured in tens of milliseconds.
    //
    // Market data does not wait on it: the adapter is polled every pass either
    // way, and a thousand passes a second against a venue delivering some five
    // thousand messages a second is a drain capacity of 256 000 — the poll
    // ceiling times the pass rate — so the queue never grows.
    //
    // MEASURED, okx, full catalogue: 37.8% of a core spinning, 20.7% at a
    // 200 us park, 15.6% at 1 ms, 12.9% at 5 ms. Stopped at 1 ms because the
    // 5 ms run also delivered fewer messages (72k against 100k) and market
    // drift cannot be told from drainage lag at that distance — the two points
    // of a core it buys are not worth being unable to say which.
    //
    // AND IT BUYS NO PROCESSOR, which is worth saying because the shape below
    // looks like it should. Measured A/B on the same box, same binary but for
    // this branch, 2026-09-18: the park cost 8.37% of a core per thousand
    // packets a second and the reactor wait 8.44% — one number inside the
    // other's noise. The park had already taken the cost out; what is left is
    // receiving, decrypting and parsing sixteen thousand messages a second,
    // and no loop shape touches that.
    //
    // It stays for the OTHER half. A blind 1 ms sleep does not know a packet
    // arrived, so data waits out the rest of the tick; the reactor returns on
    // the packet. Up to a millisecond, against venue round trips of 1.5 to
    // 7.3 ms from Tokyo.
    //
    // THE PARK IS STILL NEEDED WHERE SOMETHING OTHER THAN A SOCKET CAN HAPPEN.
    // The reason the loop parks rather than blocks is the command rings: they
    // are shared memory, a reactor cannot be woken by a write to them, and a
    // cancel must not wait on a venue's silence. A gateway with no credentials
    // has no commands to miss — it rejects every one it is handed — so there
    // it waits inside epoll and wakes on the packet instead of on a timer.
    //
    // THE TIMEOUT IS A PASS RATE, NOT A LATENCY, and that is what bounds it.
    // A busy venue never reaches the timeout — the reactor returns on packets
    // — but a run_for(n) pass cannot be shorter than n, so everything the loop
    // does periodically drops to 1/n. Most of that has room: the heartbeat's
    // threshold is 200 ms, the journal's group commit 50, and every adapter
    // paces its subscribe frames at 150.
    //
    // Two gates have no room. binance's depth seeding is spaced at 25 ms and
    // gated on the clock, so a 20 ms pass turns it into 40 and takes the seed
    // rate down by a third — on the one venue whose books were already losing
    // their seeds faster than the pool restored them. htx paces its subscribe
    // frames at 20 ms, which a 20 ms pass halves. Those two are the tightest
    // in any adapter; 5 ms serves both at their intended rate and still drops
    // 199 of every 200 wakeups the 1 ms park was spending.
    constexpr std::uint32_t k_spin_passes = 64;
    constexpr std::uint64_t k_park_ns = 1'000'000;
    constexpr std::uint32_t k_wait_ms = 5;
    std::uint32_t idle_passes = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto mono = clock.mono();
        if (mono >= t_end) {
            // SET, not just broken out of. The account poller below exits on
            // this flag and nothing else, and the join at the bottom waits for
            // it — so a deadline that only broke the loop hung the process
            // forever on every venue whose poller started.
            g_stop.store(true, std::memory_order_relaxed);
            break;
        }
        // Cancels first: the escape hatch never queues behind new risk.
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
            // Refused, never dropped. A command that vanishes leaves a strategy
            // waiting on a verdict that is not coming; a rejection reaches a
            // terminal state and the order store retires it.
            if (!can_trade) {
                sink.reject(cmd, "venue is running market-data only");
                continue;
            }
            // No reconciliation halt here. It used to sit on Create only,
            // which let an Amend through on an instrument the halt existed to
            // freeze. The halt now lives in strategyd's risk gate, which sees
            // every command kind and is the only writer these rings have.
            switch (cmd.kind) {
            case CmdKind::Create:
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
        // The order path needs the same heartbeat the market-data path gets. A
        // gateway that only polls data keeps a warm socket for quotes and a
        // cold one for orders, and only finds out which at the worst moment.
        adapter->execution->poll(clock.wall());
        // THE ACCOUNT PULL IS NOT HERE ANY MORE. It used to sit between two
        // heartbeats and took as long as a round trip to the venue — measured
        // at 460 ms against bybit, against a 200 ms staleness threshold — so
        // every ordinary reconciliation made this process look dead to every
        // peer, and strategyd correctly stopped. The heartbeat was the loud
        // symptom; the quiet one was worse, because for that same 460 ms the
        // command rings below were not being drained either and a person's
        // order sat waiting on an HTTP response it had nothing to do with.
        owner->beat(mono.ns);
        if (mono.ns >= flush_due) {
            journal_writer->flush();
            // BESIDE THE FLUSH, not beside the heartbeat: the count changes a
            // handful of times in a run and the heartbeat runs every pass.
            owner->publish_refusals(adapter->data->subscribe_refusals());
            flush_due = mono.ns + 50'000'000; // 50 ms group commit
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
    adapter->data->stop();
    adapter->execution->stop();
    journal_writer->flush();
    const auto& counts = sink.counts();
    requests.report(account_name);
    rates.report(account_name);
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
