// compiled with nothing but the sdk's headers in front of it: a boost or glaze type in them would not build for a user
#include <raikiri/sdk/client.hpp>
#include <raikiri/sdk/decimal.hpp>
#include <raikiri/sdk/models.hpp>

#ifdef BOOST_VERSION
#error "a public header pulled in boost"
#endif
#ifdef GLZ_VERSION
#error "a public header pulled in glaze"
#endif

#include <gtest/gtest.h>

#include <concepts>

using raikiri::sdk::Decimal;

static_assert(!std::constructible_from<Decimal, double>);
static_assert(!std::constructible_from<Decimal, float>);
static_assert(std::constructible_from<Decimal, int>);

TEST(PublicHeaders, CompileAlone) {
    SUCCEED();
}
