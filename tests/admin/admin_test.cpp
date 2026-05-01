#include <raikiri/admin/format.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/admin/stream.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
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
    // The rule the whole contract turns on. JavaScript holds integers exactly
    // only to 2^53 — 9007199254740992 — and both of these are past it, so a
    // JSON number would arrive at the browser rounded.
    const auto id = InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{3});
    EXPECT_GT(id.packed, 9'007'199'254'740'992ULL);
    EXPECT_EQ(digits(id), std::to_string(id.packed));

    const UnixNanos ts{1'787'904'000'000'000'000ULL};
    EXPECT_GT(ts.ns, 9'007'199'254'740'992ULL);
    EXPECT_EQ(digits(ts), "1787904000000000000");

    // And they are DIGITS, so a reader can parse them without guessing.
    for (const char c : digits(id)) {
        EXPECT_TRUE(c >= '0' && c <= '9');
    }
}

TEST(AdminFormat, APriceIsResolvedThroughTheInstrumentThatGivesItMeaning) {
    // A tick count without its instrument is not a price. 40000 at a 0.0001
    // tick is 400 000 000 ticks, and rendering the count instead of the price
    // would be off by four orders of magnitude.
    const auto base = base_of_kind(MarketType::Option, "BTC-28AUG26-40000-C");
    const auto strike = *base.price_to_ticks(*parse_decimal_e9("40000"));
    EXPECT_EQ(strike.raw, 400'000'000);
    EXPECT_EQ(price(strike, base), "40000");
    EXPECT_EQ(size(*base.qty_to_lots(*parse_decimal_e9("2.5")), base), "2.5");
}

TEST(AdminFormat, ASizePastInt64AtE9IsStillRendered) {
    // Phemex spot PEPE/USDT, lot 1, quoted 20 755 471 406 at the ask.
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
    // Absent, never zero: an expiry of zero is a date in 1970 and a strike of
    // zero is a free option, and both would be read as data.
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

    // And absent means ABSENT on the wire, not null.
    std::string body;
    ASSERT_FALSE(glz::write<glz::opts{.skip_null_members = true}>(flat, body));
    EXPECT_EQ(body.find("strike"), std::string::npos) << body;
    EXPECT_EQ(body.find("null"), std::string::npos) << body;
}

TEST(AdminRegistry, AnEmptyRuntimeDirectoryIsNoGatewaysRatherThanAnError) {
    // Gateways come and go. A terminal that can say "none are running" is more
    // useful than one that refuses to start.
    Registry registry;
    EXPECT_EQ(registry.discover("/tmp/raikiri-admin-test-does-not-exist", 1), 0U);
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_TRUE(registry.accounts(MonoNanos{1}, UnixNanos{1}).empty());
    EXPECT_EQ(registry.find("bybit"), nullptr);
    EXPECT_FALSE(registry.instrument(InstrumentId{1}).has_value());
}

TEST(AdminServer, QueryParametersAreReadByNameAndNotByPosition) {
    constexpr std::string_view query = "account=bybit-spot&kind=option&limit=50";
    EXPECT_EQ(http::query_param(query, "account"), "bybit-spot");
    EXPECT_EQ(http::query_param(query, "kind"), "option");
    EXPECT_EQ(http::query_param(query, "limit"), "50");
    EXPECT_TRUE(http::query_param(query, "cursor").empty());
    // A prefix is not a name: `limit` must not answer for `lim`.
    EXPECT_TRUE(http::query_param(query, "lim").empty());
    EXPECT_TRUE(http::query_param("", "account").empty());
    EXPECT_TRUE(http::query_param("account", "account").empty()) << "a key with no value is no answer";
}

// ------------------------------------------------- a restarted gateway ----
//
// Against REAL segments, because the defect is in what the registry does with a
// second segment under the same name and a hand-built attachment would prove
// nothing about the mapping that goes away.

std::string segment_dir() {
    return "/tmp/raikiri-admin-reattach-" + std::to_string(::getpid());
}

// Removed at the end whatever the test does, so a failed assertion cannot leave
// a segment behind for the next run to attach to.
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

// The catalogue hands out slots in push order, so an instrument's SymbolId is
// the slot it will land in.
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

// One run of one gateway, in gatewayd's own order: create the segment, fill the
// catalogue, and only then publish — so a reader that can attach at all can
// resolve every id it will be sent.
std::optional<shm::SegmentOwner> start_gateway(std::uint64_t generation,
                                               const std::vector<std::string>& symbols) {
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

TEST(AdminReattach, AGatewayThatRestartedIsAttachedAgainRatherThanLeftForDead) {
    // MEASURED: a gatewayd was killed and started again, loaded its instruments
    // and subscribed, and /v1/health went on reporting that account alive=false
    // with the dead run's generation until admind itself was restarted.
    const SegmentDir dir;
    constexpr std::uint64_t k_first_run = 1'000;
    // Far enough past the first run's last heartbeat that the dead owner reads
    // dead, which is exactly what the terminal showed.
    constexpr std::uint64_t k_second_run = k_first_run + 10 * shm::k_owner_stale_ns;
    // A plausible wall clock: the two are unrelated readings, which is the
    // whole point — the monotonic one is small because the machine booted
    // recently, the epoch one is not.
    constexpr std::uint64_t k_wall_now = 1'788'000'000'000'000'000ULL;

    auto first_run = start_gateway(k_first_run, {"AAA"});
    ASSERT_TRUE(first_run.has_value());

    Registry registry;
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    const Gateway* attached = registry.find("sim");
    ASSERT_NE(attached, nullptr);
    EXPECT_EQ(attached->client.generation(), k_first_run);
    const auto epoch = registry.epoch();

    // A rescan of the SAME run must change nothing: re-attaching a live gateway
    // every two seconds would spend the segment's four reader slots and unmap a
    // catalogue nobody asked it to.
    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U);
    EXPECT_EQ(registry.find("sim"), attached);
    EXPECT_EQ(registry.epoch(), epoch);

    // The gateway dies and comes back: a new segment, a new generation, and a
    // catalogue of its own. This is the rescan that used to skip it.
    first_run.reset();
    const auto second_run = start_gateway(k_second_run, {"BBB"});
    ASSERT_TRUE(second_run.has_value());

    const auto dead = registry.accounts(MonoNanos{k_second_run}, UnixNanos{k_wall_now});
    // THE HEARTBEAT LEAVES ON THE WALL CLOCK. The gateway stamps it on the
    // monotonic one and that is right for `alive` above, but the contract calls
    // this field nanoseconds since the epoch — reported raw, twelve days of
    // uptime read as 1969 and the terminal drew "496579h ago".
    EXPECT_LE(std::stoull(dead.front().gateway.last_heartbeat), k_wall_now);
    EXPECT_GT(std::stoull(dead.front().gateway.last_heartbeat), k_wall_now / 2)
        << "dated by its age, not by the monotonic reading";
    ASSERT_EQ(dead.size(), 1U);
    EXPECT_FALSE(dead.front().gateway.alive) << "the dead run, still attached";
    EXPECT_EQ(dead.front().gateway.generation, digits(k_first_run));

    ASSERT_EQ(registry.discover(segment_dir(), 1), 1U) << "one account, not two";
    EXPECT_EQ(registry.find("sim"), attached) << "the Gateway keeps its address (registry.hpp)";
    EXPECT_EQ(attached->client.generation(), k_second_run);
    EXPECT_GT(registry.epoch(), epoch) << "every pointer into the old mapping is stale";

    const auto live = registry.accounts(MonoNanos{k_second_run}, UnixNanos{k_wall_now});
    ASSERT_EQ(live.size(), 1U);
    EXPECT_TRUE(live.front().gateway.alive);
    EXPECT_EQ(live.front().gateway.generation, digits(k_second_run));

    // And every read now goes to the NEW catalogue.
    const auto instruments = registry.instruments("sim", std::nullopt);
    ASSERT_EQ(instruments.size(), 1U);
    EXPECT_EQ(instruments.front().exchange_symbol, "BBB");
}

TEST(AdminReattach, ATargetBelongsToTheAttachmentItWasResolvedAgainst) {
    // The hazard the re-attach creates. A live subscription holds a `listed`
    // POINTING INTO the mapping a re-attach drops, so a session compares
    // Registry::epoch() against the one it resolved at and resolves again; this
    // is what resolving again has to answer.
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
    // Kept as a VALUE and never read again: once the re-attach has happened it
    // is a pointer into an unmapped segment, and reading one is the
    // use-after-free this test exists to keep out.
    const auto* dropped = before.front().listed;
    const auto epoch = registry.epoch();

    // The gateway comes back listing one of the two.
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
    // The one the restarted run no longer lists is SAID rather than quietly
    // dropped: a feed that stops ticking without a word is a terminal showing
    // nothing where a price was asked for.
    ASSERT_EQ(missing.size(), 1U);
    EXPECT_EQ(missing.front(), digits(gone));
}

} // namespace
} // namespace raikiri::admin
