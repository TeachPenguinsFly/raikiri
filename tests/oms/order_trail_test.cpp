#include <raikiri/audit/file_sink.hpp>
#include <raikiri/audit/verify.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/oms/order_trail.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <glaze/glaze.hpp>
#include <map>
#include <string>
#include <vector>

// counts this thread's allocations, so the sink's own thread does not show
namespace {
thread_local std::uint64_t t_allocations = 0;
} // namespace

// gcc pairs this malloc with the libc free below and reads a mismatch once they inline together
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    ++t_allocations;
    if (void* block = std::malloc(size); block != nullptr) {
        return block;
    }
    throw std::bad_alloc{};
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace raikiri::oms {
namespace {

using E = OrderEventType;

constexpr InstrumentId k_instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
constexpr ClientOrderId k_id = ClientOrderId::make(7, 1);
constexpr UnixNanos k_now{1'000'000'000'000'000'000ULL};

struct Kept {
    std::string actor;
    std::string action;
    std::string subject;
    std::string detail;
};

class Keeping final : public audit::Sink {
public:
    void append(const audit::Entry& entry) override {
        kept.push_back({std::string{entry.actor},
                        std::string{entry.action},
                        std::string{entry.subject},
                        std::string{entry.detail_json}});
    }

    std::vector<Kept> kept;
};

class Silent final : public audit::Sink {
public:
    void append(const audit::Entry& entry) noexcept override { bytes += entry.detail_json.size(); }

    std::size_t bytes{0};
};

OrderCmd create_cmd() {
    OrderCmd cmd{};
    cmd.instrument = k_instrument;
    cmd.client_order_id = k_id;
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01JABCDEFGHJKMNPQRSTVWXYZ0");
    cmd.price = PriceTicks::from_raw(100);
    cmd.qty = QtyLots::from_raw(10);
    cmd.ts_created = k_now;
    return cmd;
}

OrderEvent event_of(E type) {
    OrderEvent out{};
    out.type = type;
    out.instrument = k_instrument;
    out.client_order_id = k_id;
    out.exchange_client_order_id = create_cmd().exchange_client_order_id;
    out.exchange_order_id = *ExchangeOrderId::from("venue\"id");
    out.price = PriceTicks::from_raw(101);
    out.qty = QtyLots::from_raw(3);
    out.leaves_qty = QtyLots::from_raw(7);
    out.exec_id = *ExecId::from("exec-1");
    out.fee = FixedE9::from_raw(1'500'000);
    out.fee_asset = to_fee_asset("USDT");
    out.ts_event = k_now;
    out.ts_init = k_now;
    return out;
}

TEST(OrderTrail, EveryStepOfALifeHasItsOwnAction) {
    Keeping sink;
    OrderTrail trail{sink};
    Order order{create_cmd(), create_cmd().exchange_client_order_id};
    trail.command("order.created", create_cmd(), k_now);
    trail.command("order.sent", create_cmd(), k_now);
    for (const auto type :
         {E::Accepted, E::Updated, E::Filled, E::PendingCancel, E::Canceled, E::Rejected, E::ReconClosed}) {
        trail.event(event_of(type), &order, k_now);
    }
    std::vector<std::string> actions;
    for (const auto& one : sink.kept) {
        actions.push_back(one.action);
        EXPECT_EQ(one.subject, "order:" + std::to_string(k_id.raw));
        EXPECT_EQ(one.actor, "strategy:7");
        EXPECT_FALSE(glz::validate_json(one.detail)) << one.detail;
    }
    EXPECT_EQ(actions,
              (std::vector<std::string>{"order.created",
                                        "order.sent",
                                        "order.acked",
                                        "order.amended",
                                        "order.filled",
                                        "order.cancel_sent",
                                        "order.cancelled",
                                        "order.rejected",
                                        "order.recon_closed"}));
}

TEST(OrderTrail, AFillNamesItsBookIdsPriceAndSize) {
    Keeping sink;
    OrderTrail trail{sink};
    Order order{create_cmd(), create_cmd().exchange_client_order_id};
    trail.event(event_of(E::Filled), &order, k_now);
    const auto& detail = sink.kept.at(0).detail;
    for (const auto* part : {R"("book":"7")",
                             R"("price_ticks":"101")",
                             R"("qty_lots":"3")",
                             R"("exec_id":"exec-1")",
                             R"("fee":"0.0015")",
                             R"("fee_asset":"USDT")",
                             R"("exchange_order_id":"venue?id")",
                             R"("exchange_client_order_id":"01JABCDEFGHJKMNPQRSTVWXYZ0")"}) {
        EXPECT_NE(detail.find(part), std::string::npos) << part << " not in " << detail;
    }
}

TEST(OrderTrail, AHandPlacedOrderIsTheDesks) {
    Keeping sink;
    OrderTrail trail{sink};
    auto cmd = create_cmd();
    cmd.client_order_id = ClientOrderId::make(k_manual_strategy_id, 5);
    trail.command("order.created", cmd, k_now);
    EXPECT_EQ(sink.kept.at(0).actor, "desk");
}

TEST(OrderTrail, TheOrderPathAllocatesNothing) {
    Silent sink;
    OrderTrail trail{sink};
    const Order order{create_cmd(), create_cmd().exchange_client_order_id};
    const auto cmd = create_cmd();
    const auto event = event_of(E::Filled);
    const auto before = t_allocations;
    trail.command("order.created", cmd, k_now);
    trail.event(event, &order, k_now);
    trail.event(event, nullptr, k_now);
    EXPECT_EQ(t_allocations, before);
    EXPECT_GT(sink.bytes, 0U);
}

TEST(OrderTrail, ReachesTheChainWholeThroughTheFileSink) {
    const auto dir = std::filesystem::temp_directory_path() / "order_trail_chain";
    std::filesystem::remove_all(dir);
    const raikiri::log::Scope logging{"order_trail_test"};
    const LiveClock clock;
    {
        auto sink = audit::FileSink::open(dir.string(), {}, clock);
        ASSERT_TRUE(sink.has_value());
        OrderTrail trail{**sink};
        Order order{create_cmd(), create_cmd().exchange_client_order_id};
        const auto now = clock.wall();
        for (int i = 0; i < 2000; ++i) {
            trail.event(event_of(E::Filled), &order, now);
            if (i % 512 == 0) {
                (*sink)->wait_drained();
            }
        }
        (*sink)->wait_drained();
        EXPECT_EQ((*sink)->stats().unparsed_details, 0U);
    }
    const auto reports = audit::verify_all(dir.string());
    ASSERT_FALSE(reports.empty());
    EXPECT_TRUE(reports.back().ok());
    std::filesystem::remove_all(dir);
}

} // namespace
} // namespace raikiri::oms
