#include <raikiri/admin/desk.hpp>
#include <raikiri/admin/format.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/routes.hpp>
#include <raikiri/admin/stream.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace raikiri::admin {
namespace {

InstrumentBase base_of_kind(MarketType market, std::string_view symbol) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Deribit, market, SymbolId{3});
    base.exchange_symbol = *SymbolText::from(symbol);
    base.local_symbol = *SymbolText::from(symbol);
    base.tick_size = *parse_decimal_e9("0.0001");
    base.lot_size = *parse_decimal_e9("0.1");
    base.min_notional = *parse_decimal_e9("0.1");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return base;
}

TEST(AdminFormat, EveryValueTooLargeForADoubleLeavesAsDigits) {
    // both ids exceed 2^53, so a json number would round them
    const auto id = InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{3});
    EXPECT_GT(id.packed, 9'007'199'254'740'992ULL);
    EXPECT_EQ(digits(id), std::to_string(id.packed));

    const UnixNanos ts{1'787'904'000'000'000'000ULL};
    EXPECT_GT(ts.ns, 9'007'199'254'740'992ULL);
    EXPECT_EQ(digits(ts), "1787904000000000000");

    for (const char c : digits(id)) {
        EXPECT_TRUE(c >= '0' && c <= '9');
    }
}

TEST(AdminFormat, ATurnoverPastInt64AtE9IsStillANumber) {
    EXPECT_EQ(decimal(parse_turnover_e6("10666647192.81728").value_or(FixedE6::undef())), "10666647192.81728");
    EXPECT_EQ(decimal(FixedE6::from_raw(5'000'000)), "5");
    EXPECT_EQ(decimal(FixedE6::undef()), "");
}

TEST(AdminFormat, APriceIsResolvedThroughTheInstrumentThatGivesItMeaning) {
    const auto base = base_of_kind(MarketType::Option, "BTC-28AUG26-40000-C");
    const auto strike = *base.price_to_ticks(*parse_decimal_e9("40000"));
    EXPECT_EQ(strike.raw, 400'000'000);
    EXPECT_EQ(price(strike, base), "40000");
    EXPECT_EQ(size(*base.qty_to_lots(*parse_decimal_e9("2.5")), base), "2.5");
}

TEST(AdminFormat, ASizePastInt64AtE9IsStillRendered) {
    auto base = base_of_kind(MarketType::Spot, "PEPE/USDT");
    base.lot_size = *parse_decimal_e9("1");
    base.tick_size = *parse_decimal_e9("0.00000001");
    EXPECT_EQ(size(QtyLots::from_raw(20'755'471'406), base), "20755471406");
    EXPECT_EQ(size(QtyLots::from_raw(-20'755'471'406), base), "-20755471406");
    EXPECT_EQ(price(PriceTicks::from_raw(444), base), "0.00000444");
    EXPECT_EQ(price(PriceTicks::from_raw(0), base), "0");
    EXPECT_EQ(size(QtyLots::undef(), base), "");
}

TEST(AdminRegistry, AnOptionCarriesItsStrikeAndExpiryAndASpotCarriesNeither) {
    const auto base = base_of_kind(MarketType::Option, "BTC-28AUG26-40000-C");
    const OptionInstrument option{.base = base,
                                  .strike = *base.price_to_ticks(*parse_decimal_e9("40000")),
                                  .expiry = UnixNanos{1'787'904'000'000'000'000ULL},
                                  .put_call = PutCall::Put};
    const auto described = describe(raikiri::Instrument{option}, "deribit-opt");
    EXPECT_EQ(described.market, "option");
    EXPECT_EQ(described.strike, "40000");
    EXPECT_EQ(described.expiry, "1787904000000000000");
    EXPECT_EQ(described.put_call, "put");
    EXPECT_EQ(described.contract_kind, "linear");

    const SpotInstrument spot{base_of_kind(MarketType::Spot, "BTCUSDT")};
    const auto flat = describe(raikiri::Instrument{spot}, "bybit-spot");
    EXPECT_FALSE(flat.strike.has_value());
    EXPECT_FALSE(flat.expiry.has_value());
    EXPECT_FALSE(flat.put_call.has_value());

    std::string body;
    ASSERT_FALSE(glz::write<glz::opts{.skip_null_members = true}>(flat, body));
    EXPECT_EQ(body.find("strike"), std::string::npos) << body;
    EXPECT_EQ(body.find("null"), std::string::npos) << body;
}

TEST(AdminRegistry, AnEmptyRuntimeDirectoryIsNoGatewaysRatherThanAnError) {
    Registry registry;
    EXPECT_EQ(registry.discover("/tmp/raikiri-admin-test-does-not-exist", 1), 0U);
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_TRUE(registry.accounts(MonoNanos{1}, UnixNanos{1}).empty());
    EXPECT_EQ(registry.find("bybit"), nullptr);
    EXPECT_FALSE(registry.instrument(InstrumentId{1}).has_value());
}

TEST(HttpServer, ARequestIsAnsweredWhileTheServerBlocks) {
    http::Server server{"127.0.0.1", 0, [](const http::Request&) {
                            return http::Response{.status = 200, .content_type = "application/json", .body = "{}"};
                        }};
    ASSERT_TRUE(server.start());
    std::atomic<bool> done{false};
    std::thread runner{[&] {
        while (!done.load()) {
            (void)server.run_for(std::chrono::milliseconds{500});
        }
    }};
    const auto sent = std::chrono::steady_clock::now();
    const auto got = http::fetch("127.0.0.1", server.port(), "/v1/health", 2000);
    const auto took = std::chrono::steady_clock::now() - sent;
    done = true;
    runner.join();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, 200);
    EXPECT_LT(took, std::chrono::milliseconds{100});
}

TEST(AdminServer, QueryParametersAreReadByNameAndNotByPosition) {
    constexpr std::string_view query = "account=bybit-spot&kind=option&limit=50";
    EXPECT_EQ(http::query_param(query, "account"), "bybit-spot");
    EXPECT_EQ(http::query_param(query, "kind"), "option");
    EXPECT_EQ(http::query_param(query, "limit"), "50");
    EXPECT_TRUE(http::query_param(query, "cursor").empty());
    EXPECT_TRUE(http::query_param(query, "lim").empty());
    EXPECT_TRUE(http::query_param("", "account").empty());
    EXPECT_TRUE(http::query_param("account", "account").empty()) << "a key with no value is no answer";
}

std::string segment_dir() {
    return "/tmp/raikiri-admin-reattach-" + std::to_string(::getpid());
}

// removed on exit so a failed assertion leaves no segment behind
struct SegmentDir {
    SegmentDir() {
        std::error_code ec;
        std::filesystem::remove_all(segment_dir(), ec);
        std::filesystem::create_directories(segment_dir(), ec);
    }

    SegmentDir(const SegmentDir&) = delete;
    SegmentDir& operator=(const SegmentDir&) = delete;

    ~SegmentDir() {
        std::error_code ec;
        std::filesystem::remove_all(segment_dir(), ec);
    }
};

// the catalogue assigns slots in push order, so a SymbolId is its slot
raikiri::Instrument listed_as(std::uint32_t slot, std::string_view symbol) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{slot});
    base.exchange_symbol = *SymbolText::from(symbol);
    base.local_symbol = *SymbolText::from(symbol);
    base.tick_size = *parse_decimal_e9("0.01");
    base.lot_size = *parse_decimal_e9("0.001");
    base.min_notional = *parse_decimal_e9("1");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return raikiri::Instrument{PerpInstrument{.base = base}};
}

// same order as gatewayd: create the segment, fill the catalogue, then publish
std::optional<shm::SegmentOwner> start_gateway(std::uint64_t generation, const std::vector<std::string>& symbols) {
    auto owner = shm::SegmentOwner::create(
        segment_dir() + "/gw.sim", shm::k_gateway_segment_bytes, generation, generation);
    if (!owner.has_value()) {
        return std::nullopt;
    }
    const auto channels = shm::create_gateway_channels(*owner);
    if (!channels.has_value()) {
        return std::nullopt;
    }
    for (std::uint32_t slot = 0; slot < symbols.size(); ++slot) {
        if (!channels->catalogue->push(listed_as(slot, symbols[slot])).has_value()) {
            return std::nullopt;
        }
    }
    owner->publish();
    return std::optional<shm::SegmentOwner>{std::move(*owner)};
}

TEST(AdminReattach, AHeartbeatStampedAfterTheRequestReadItsClockIsFreshNotZero) {
    const SegmentDir dir;
    constexpr std::uint64_t k_beat = 5'000;
    constexpr std::uint64_t k_wall_now = 1'788'000'000'000'000'000ULL;
    const auto gateway = start_gateway(k_beat, {"AAA"});
    ASSERT_TRUE(gateway.has_value());

    Registry registry;
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    // the gateway beat between admind reading `now` and reading the segment
    const auto rows = registry.accounts(MonoNanos{k_beat - 1}, UnixNanos{k_wall_now});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.front().gateway.last_heartbeat, digits(k_wall_now)) << "read as never stamped, 56 years ago";
}

TEST(AdminReattach, AGatewayThatRestartedIsAttachedAgainRatherThanLeftForDead) {
    const SegmentDir dir;
    constexpr std::uint64_t k_first_run = 1'000;
    constexpr std::uint64_t k_second_run = k_first_run + 10 * shm::k_owner_stale_ns;
    constexpr std::uint64_t k_wall_now = 1'788'000'000'000'000'000ULL;

    auto first_run = start_gateway(k_first_run, {"AAA"});
    ASSERT_TRUE(first_run.has_value());

    Registry registry;
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    const Gateway* attached = registry.find("sim");
    ASSERT_NE(attached, nullptr);
    EXPECT_EQ(attached->client.generation(), k_first_run);
    const auto epoch = registry.epoch();

    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    EXPECT_EQ(registry.find("sim"), attached);
    EXPECT_EQ(registry.epoch(), epoch);

    first_run.reset();
    const auto second_run = start_gateway(k_second_run, {"BBB"});
    ASSERT_TRUE(second_run.has_value());

    const auto dead = registry.accounts(MonoNanos{k_second_run}, UnixNanos{k_wall_now});
    // last_heartbeat is reported on the wall clock
    EXPECT_LE(std::stoull(dead.front().gateway.last_heartbeat), k_wall_now);
    EXPECT_GT(std::stoull(dead.front().gateway.last_heartbeat), k_wall_now / 2)
        << "dated by its age, not by the monotonic reading";
    ASSERT_EQ(dead.size(), 1U);
    EXPECT_FALSE(dead.front().gateway.alive) << "the dead run, still attached";
    EXPECT_EQ(dead.front().gateway.generation, digits(k_first_run));

    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U) << "one account, not two";
    EXPECT_EQ(registry.find("sim"), attached) << "the Gateway keeps its address (registry.hpp)";
    EXPECT_TRUE(attached->lease.has_value()) << "the restart keeps the desk's lease";
    EXPECT_EQ(attached->client.generation(), k_second_run);
    EXPECT_GT(registry.epoch(), epoch) << "every pointer into the old mapping is stale";

    const auto live = registry.accounts(MonoNanos{k_second_run}, UnixNanos{k_wall_now});
    ASSERT_EQ(live.size(), 1U);
    EXPECT_TRUE(live.front().gateway.alive);
    EXPECT_EQ(live.front().gateway.generation, digits(k_second_run));

    const auto instruments = registry.instruments("sim", std::nullopt);
    ASSERT_EQ(instruments.size(), 1U);
    EXPECT_EQ(instruments.front().exchange_symbol, "BBB");
}

TEST(AdminDeskLease, ASecondAdmindIsRefusedTheIntentRingRatherThanSharingIt) {
    const SegmentDir dir;
    const auto run = start_gateway(1'000, {"AAA"});
    ASSERT_TRUE(run.has_value());

    std::optional<Registry> first{std::in_place};
    ASSERT_EQ(first->discover(segment_dir(), 1), 1U);
    EXPECT_TRUE(first->find("sim")->lease.has_value());

    Registry second;
    ASSERT_EQ(second.discover(segment_dir(), 1), 1U) << "still reads the gateway";
    EXPECT_FALSE(second.find("sim")->lease.has_value());

    ManualDesk desk{second};
    const DeskOrder order{
        .instrument = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{0}), .price = "1.00", .qty = "0.001"};
    const auto refused = desk.place(order, "", UnixNanos{1});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), DeskError::RingHeld);

    first.reset();
    ASSERT_EQ(second.discover(segment_dir(), 1), 1U);
    EXPECT_TRUE(second.find("sim")->lease.has_value()) << "taken up once the first admind is gone";
    EXPECT_TRUE(desk.place(order, "", UnixNanos{2}).has_value());
}

TEST(AdminReattach, AStoppedGatewayIsLetGoOnceItsFileIsGone) {
    const SegmentDir dir;
    auto run = start_gateway(1'000, {"AAA"});
    ASSERT_TRUE(run.has_value());

    Registry registry;
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    const auto epoch = registry.epoch();
    run.reset();
    ASSERT_FALSE(std::filesystem::exists(segment_dir() + "/gw.sim"));

    EXPECT_EQ(registry.discover(segment_dir(), 1), 0U) << "an unlinked segment is not held mapped";
    EXPECT_EQ(registry.find("sim"), nullptr);
    EXPECT_GT(registry.epoch(), epoch);
}

TEST(AdminReattach, ATargetBelongsToTheAttachmentItWasResolvedAgainst) {
    const SegmentDir dir;
    const auto kept = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{0});
    const auto gone = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1});
    const std::vector<InstrumentId> watchlist{kept, gone};

    auto first_run = start_gateway(1'000, {"AAA", "BBB"});
    ASSERT_TRUE(first_run.has_value());

    Registry registry;
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    std::vector<std::string> unknown;
    const auto before = resolve_targets(registry, watchlist, unknown);
    ASSERT_TRUE(unknown.empty());
    ASSERT_EQ(before.size(), 2U);
    // dangles after the re-attach; never dereference it
    const auto* dropped = before.front().listed;
    const auto epoch = registry.epoch();

    first_run.reset();
    const auto second_run = start_gateway(2'000, {"AAA"});
    ASSERT_TRUE(second_run.has_value());
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    ASSERT_NE(registry.epoch(), epoch) << "the compare a live session re-resolves on";

    std::vector<std::string> missing;
    const auto after = resolve_targets(registry, watchlist, missing);
    ASSERT_EQ(after.size(), 1U);
    EXPECT_NE(after.front().listed, dropped) << "a row of the new mapping, not the old one";
    EXPECT_EQ(after.front().listed, registry.find("sim")->channels.catalogue->find(kept));
    EXPECT_EQ(after.front().id, kept);
    EXPECT_EQ(after.front().cell, 0U);
    ASSERT_EQ(missing.size(), 1U);
    EXPECT_EQ(missing.front(), digits(gone));
}

TEST(AdminVenueMetrics, TheRouteReadsTheClockAndFeedEachGatewayPublished) {
    const SegmentDir dir;
    constexpr std::uint64_t k_run = 1'000;
    constexpr std::uint64_t k_wall_now = 1'788'000'000'000'000'000ULL;
    constexpr std::uint64_t k_ms = 1'000'000;
    const auto gateway = start_gateway(k_run, {"AAA", "BBB"});
    ASSERT_TRUE(gateway.has_value());
    Registry registry;
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    const auto router = make_router(
        registry, nullptr, nullptr, [] { return MonoNanos{k_run}; }, [] { return UnixNanos{k_wall_now}; });
    const http::Request request{.method = "GET", .path = "/v1/venues/metrics"};

    auto answer = router(request);
    ASSERT_EQ(answer.status, 200);
    EXPECT_NE(answer.body.find(R"("account":"sim")"), std::string::npos) << answer.body;
    EXPECT_NE(answer.body.find(R"("method":"none")"), std::string::npos) << answer.body;
    EXPECT_EQ(answer.body.find("offset_ns"), std::string::npos) << "no clock, no offset: " << answer.body;
    EXPECT_NE(answer.body.find(R"("offset_source":"none")"), std::string::npos) << answer.body;
    EXPECT_NE(answer.body.find(R"("lag_shown":"raw")"), std::string::npos) << answer.body;
    EXPECT_EQ(answer.body.find("offset_low_ns"), std::string::npos) << answer.body;

    shm::VenueMetrics m{};
    m.published_ns = k_wall_now - 2'000 * k_ms;
    m.clock_source = shm::ClockSource::RestProbe;
    m.offset_ns = -7 * static_cast<std::int64_t>(k_ms);
    m.rtt_ns = 180 * k_ms;
    m.uncertainty_ns = 90 * k_ms + k_ms / 2;
    m.measured_ns = k_wall_now - 60'000 * k_ms;
    m.drift_ns = static_cast<std::int64_t>(k_ms);
    m.drift_over_ns = 3'600'000 * k_ms;
    m.offset_source = shm::OffsetSource::RestAndFeed;
    m.feed_bounded = 1;
    m.feed_bound_ns = 2 * static_cast<std::int64_t>(k_ms);
    m.offset_lo_ns = 2 * static_cast<std::int64_t>(k_ms);
    m.offset_hi_ns = 83 * static_cast<std::int64_t>(k_ms) + static_cast<std::int64_t>(k_ms / 2);
    m.correction_ns = 2 * static_cast<std::int64_t>(k_ms);
    m.catalogue = 2;
    m.priced = 1;
    m.reopens = 3;
    const std::string_view endpoint = "fapi.binance.com/fapi/v1/time";
    std::copy(endpoint.begin(), endpoint.end(), m.endpoint);
    m.channels[0] = {.messages = 600,
                     .window_ns = 60'000 * k_ms,
                     .lag_samples = 600,
                     .lag_p50_ns = 12 * static_cast<std::int64_t>(k_ms),
                     .lag_p99_ns = 40 * static_cast<std::int64_t>(k_ms),
                     .lag_min_ns = -2 * static_cast<std::int64_t>(k_ms),
                     .last_recv_ns = k_wall_now - 5 * k_ms};
    registry.find("sim")->channels.metrics->write(m);

    answer = router(request);
    ASSERT_EQ(answer.status, 200);
    std::vector<VenueMetrics> rows;
    ASSERT_FALSE(glz::read_json(rows, answer.body)) << answer.body;
    ASSERT_EQ(rows.size(), 1U);
    const auto& row = rows.front();
    EXPECT_EQ(row.venue, "sim");
    EXPECT_TRUE(row.alive);
    EXPECT_EQ(row.clock.method, "rest_probe");
    EXPECT_EQ(row.clock.endpoint, endpoint);
    EXPECT_EQ(row.clock.offset_ns, -7 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(row.clock.uncertainty_ns, static_cast<std::int64_t>(90 * k_ms + k_ms / 2));
    EXPECT_EQ(row.clock.measured_at, digits(k_wall_now - 60'000 * k_ms));
    EXPECT_EQ(row.clock.measured_ago_ns, static_cast<std::int64_t>(60'000 * k_ms));
    EXPECT_EQ(row.clock.drift_ns, static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(row.clock.offset_source, "rest_and_feed");
    EXPECT_EQ(row.clock.feed_bound_ns, 2 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(row.clock.offset_low_ns, 2 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(row.clock.offset_high_ns, m.offset_hi_ns);
    EXPECT_EQ(row.clock.correction_ns, 2 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(row.clock.lag_shown, "corrected");
    EXPECT_EQ(row.reopens, 3);
    EXPECT_EQ(row.priced, 1);
    ASSERT_EQ(row.channels.size(), 1U) << "silent channels are left out";
    const auto& bbo = row.channels.front();
    EXPECT_EQ(bbo.channel, "bbo");
    EXPECT_EQ(bbo.msgs_per_s, 10);
    EXPECT_EQ(bbo.lag_p50_ns, 12 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(bbo.lag_p50_corrected_ns, 14 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(bbo.lag_p99_corrected_ns, 42 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(bbo.lag_min_ns, -2 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(bbo.last_message_age_ns, static_cast<std::int64_t>(5 * k_ms));

    // the feed stamped above the probe's upper edge: both edges shown, nothing corrected
    m.offset_source = shm::OffsetSource::Conflict;
    m.feed_bound_ns = 90 * static_cast<std::int64_t>(k_ms);
    m.offset_lo_ns = m.feed_bound_ns;
    m.correction_ns = 0;
    registry.find("sim")->channels.metrics->write(m);
    answer = router(request);
    rows.clear();
    ASSERT_FALSE(glz::read_json(rows, answer.body)) << answer.body;
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.front().clock.offset_source, "conflict");
    EXPECT_GT(rows.front().clock.offset_low_ns, rows.front().clock.offset_high_ns);
    EXPECT_EQ(rows.front().clock.lag_shown, "raw");
    ASSERT_EQ(rows.front().channels.size(), 1U);
    EXPECT_FALSE(rows.front().channels.front().lag_p50_corrected_ns.has_value());
    EXPECT_EQ(rows.front().channels.front().lag_p50_ns, 12 * static_cast<std::int64_t>(k_ms));
}

} // namespace
} // namespace raikiri::admin
