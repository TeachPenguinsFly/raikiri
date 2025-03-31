// gatewayd — one venue per process.
//
//   gatewayd --venue=sim [--duration <sec>]
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
#include <raikiri/config/secret.hpp>
#include <raikiri/domain/demo_instruments.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/journal/detail.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/schema/codec.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/venues/bybit/bybit_venue.hpp>
#include <raikiri/venues/sim/sim_venue.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

using msgs = raikiri_journal::schema::messages;

// Adapter events -> shm channels + journal. Single-threaded by contract.
class ShmSink final : public raikiri::exec::GatewaySink {
public:
    ShmSink(raikiri::shm::GatewayChannels channels, raikiri::journal::JournalWriter& journal)
        : ch_{channels}, journal_{&journal} {}

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

    void on_funding(const raikiri::FundingUpdate& funding) noexcept override {
        ++counts_.funding;
        const auto idx = funding.instrument.symbol().raw;
        if (idx < raikiri::shm::k_funding_cells) {
            ch_.funding->write(idx, funding);
        }
    }

    void on_order_event(const raikiri::OrderEvent& event) noexcept override {
        ++counts_.order_events;
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
                m.exchangeClientOrderIdField().assign_range(
                    event.exchange_client_order_id.view());
                m.exchangeOrderIdField().assign_range(event.exchange_order_id.view());
                m.execIdField().assign_range(event.exec_id.view());
                m.price(jt::priceTicks{event.price.raw});
                m.qty(jt::qtyLots{event.qty.raw});
                m.leavesQty(jt::qtyLots{event.leaves_qty.raw});
                m.fee(jt::fixedE9{event.fee.raw});
                m.tsEvent(jt::timestampNs{event.ts_event.ns});
                m.tsInit(jt::timestampNs{event.ts_init.ns});
            });
    }

    struct Counts {
        std::uint64_t bbo{0};
        std::uint64_t trades{0};
        std::uint64_t funding{0};
        std::uint64_t order_events{0};
    };

    [[nodiscard]] const Counts& counts() const noexcept { return counts_; }

    void on_connectivity(const raikiri::exec::ConnEvent& event) noexcept override {
        (void)raikiri::schema::append_msg<msgs::ConnectivityMsg>(
            *journal_, event.ts.ns, [&](auto m) {
                namespace jt = raikiri_journal::types;
                m.venue(jt::venueIdEnum::Sim);
                m.event(static_cast<jt::connEventEnum>(event.state));
                m.streamId(event.stream_id);
                m.tsInit(jt::timestampNs{event.ts.ns});
            });
    }

private:
    raikiri::shm::GatewayChannels ch_;
    raikiri::journal::JournalWriter* journal_;
    std::uint64_t md_seq_{0};
    Counts counts_{};
};

} // namespace

int main(int argc, char** argv) {
    using namespace raikiri;

    std::string venue_name = "sim";
    std::uint64_t duration_s = 0; // 0 = run until signaled
    bool testnet = false;
    std::string api_key_ref;
    std::string api_secret_ref;
    std::vector<std::string> symbols;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg.starts_with("--venue=")) {
            venue_name = arg.substr(8);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_s = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--testnet") {
            testnet = true;
        } else if (arg.starts_with("--symbol=")) {
            symbols.emplace_back(arg.substr(9));
        } else if (arg.starts_with("--api-key=")) {
            // A REFERENCE, never a key: "env:NAME", "file:/path", "exec:cmd".
            api_key_ref = arg.substr(10);
        } else if (arg.starts_with("--api-secret=")) {
            api_secret_ref = arg.substr(13);
        }
    }
    const auto venue = venue_from_string(venue_name);
    if (venue != VenueId::Sim && venue != VenueId::Bybit) {
        fmt::print(stderr, "gatewayd: --venue must be sim or bybit\n");
        return 2;
    }
    // Credentials are optional on purpose: public market data needs none, so
    // a gateway can run read-only against a live venue with no keys present
    // anywhere on the box.
    if (!api_key_ref.empty()) {
        config::disable_core_dumps();
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const LiveClock clock;

    // shm segment (owner side).
    const auto seg_path = shm::gateway_segment_path(venue_name);
    if (!journal::detail::mkdir_p(platform::runtime_dir() + "/shm")) {
        fmt::print(stderr, "gatewayd: cannot create runtime dir\n");
        return 1;
    }
    auto owner = shm::SegmentOwner::create(seg_path, shm::k_gateway_segment_bytes,
                                           clock.mono().ns, clock.mono().ns);
    if (!owner.has_value()) {
        fmt::print(stderr, "gatewayd: segment create failed ({})\n",
                   static_cast<int>(owner.error()));
        return 1;
    }
    auto channels = shm::create_gateway_channels(*owner);
    if (!channels.has_value()) {
        return 1;
    }

    // Journal.
    const char* data_dir_env = std::getenv("RAIKIRI_DATA_DIR");
    const std::string data_dir = data_dir_env != nullptr ? data_dir_env : "raikiri-data";
    auto journal_writer =
        journal::JournalWriter::open(data_dir + "/journal/gatewayd-" + venue_name, "journal");
    if (!journal_writer.has_value()) {
        fmt::print(stderr, "gatewayd: journal open failed\n");
        return 1;
    }

    ShmSink sink{*channels, *journal_writer};

    venues::sim::register_adapter();
    venues::bybit::register_adapter();
    auto adapter = exec::make_venue({.venue = venue,
                                     .enable_perp = true,
                                     .testnet = testnet,
                                     .api_key_env = api_key_ref,
                                     .api_secret_env = api_secret_ref},
                                    sink, clock);
    if (!adapter.has_value()) {
        fmt::print(stderr, "gatewayd: adapter build failed\n");
        return 1;
    }
    if (!adapter->data->start().has_value() || !adapter->execution->start().has_value()) {
        fmt::print(stderr, "gatewayd: adapter start failed\n");
        return 1;
    }
    if (venue == VenueId::Sim) {
        for (const auto& instrument : demo::sim_instruments()) {
            const auto id = base_of(instrument).id;
            (void)adapter->data->subscribe(exec::SubKind::Bbo, id);
            (void)adapter->data->subscribe(exec::SubKind::Trades, id);
        }
    } else {
        // The catalogue must load before any subscription: an adapter cannot
        // map a single inbound frame without the tick and lot sizes, and a
        // gateway that subscribes first would silently drop everything until
        // the table filled.
        const auto catalogue = adapter->instruments->fetch_all(MarketType::Perp);
        if (!catalogue.has_value()) {
            fmt::print(stderr, "gatewayd[{}]: catalogue load failed: {} (code {})\n", venue_name,
                       catalogue.error().message.view(), catalogue.error().venue_code);
            return 1;
        }
        fmt::print("gatewayd[{}]: {} instruments loaded\n", venue_name, catalogue->size());

        std::size_t subscribed = 0;
        for (const auto& instrument : *catalogue) {
            const auto& base = base_of(instrument);
            const auto wanted =
                symbols.empty() ||
                std::any_of(symbols.begin(), symbols.end(), [&](const std::string& s) {
                    return s == base.exchange_symbol.view();
                });
            if (!wanted) {
                continue;
            }
            (void)adapter->data->subscribe(exec::SubKind::Bbo, base.id);
            (void)adapter->data->subscribe(exec::SubKind::Trades, base.id);
            (void)adapter->data->subscribe(exec::SubKind::Funding, base.id);
            ++subscribed;
        }
        if (subscribed == 0) {
            fmt::print(stderr, "gatewayd[{}]: no requested symbol matched the catalogue\n",
                       venue_name);
            return 1;
        }
        fmt::print("gatewayd[{}]: subscribed to {} instrument(s)\n", venue_name, subscribed);
    }
    owner->publish();
    fmt::print("gatewayd[{}]: up, segment {}\n", venue_name, seg_path);

    shm::SpscConsumer creates{channels->cmd_create};
    shm::SpscConsumer cancels{channels->cmd_cancel};

    const auto t_end = duration_s != 0 ? clock.mono() + duration_s * 1'000'000'000ULL
                                       : MonoNanos{~0ULL};
    std::uint64_t flush_due = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto mono = clock.mono();
        if (mono >= t_end) {
            break;
        }
        // Cancels first: the escape hatch never queues behind new risk.
        OrderCmd cmd{};
        while (cancels.try_pop(cmd)) {
            adapter->execution->cancel(cmd);
        }
        while (creates.try_pop(cmd)) {
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
        owner->beat(mono.ns);
        if (mono.ns >= flush_due) {
            journal_writer->flush();
            flush_due = mono.ns + 50'000'000; // 50 ms group commit
        }
    }

    adapter->data->stop();
    adapter->execution->stop();
    journal_writer->flush();
    const auto& counts = sink.counts();
    fmt::print("gatewayd[{}]: down — bbo={} trades={} funding={} order-events={}\n", venue_name,
               counts.bbo, counts.trades, counts.funding, counts.order_events);
    return 0;
}
