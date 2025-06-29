#include <raikiri/admin/format.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/server.hpp>

#include <glaze/glaze.hpp>
#include <gtest/gtest.h>

#include <algorithm>

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
    EXPECT_TRUE(registry.accounts(MonoNanos{1}).empty());
    EXPECT_EQ(registry.find("bybit"), nullptr);
    EXPECT_FALSE(registry.instrument(InstrumentId{1}).has_value());
}

TEST(AdminServer, QueryParametersAreReadByNameAndNotByPosition) {
    constexpr std::string_view query = "account=bybit-spot&kind=option&limit=50";
    EXPECT_EQ(query_param(query, "account"), "bybit-spot");
    EXPECT_EQ(query_param(query, "kind"), "option");
    EXPECT_EQ(query_param(query, "limit"), "50");
    EXPECT_TRUE(query_param(query, "cursor").empty());
    // A prefix is not a name: `limit` must not answer for `lim`.
    EXPECT_TRUE(query_param(query, "lim").empty());
    EXPECT_TRUE(query_param("", "account").empty());
    EXPECT_TRUE(query_param("account", "account").empty()) << "a key with no value is no answer";
}

} // namespace
} // namespace raikiri::admin
