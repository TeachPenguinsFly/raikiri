#include <raikiri/common/fixed_string.hpp>

#include <gtest/gtest.h>

#include <type_traits>
#include <unordered_set>

namespace raikiri {
namespace {

static_assert(std::is_trivially_copyable_v<FixedString<26>>,
              "FixedString must stay POD-like: it is embedded in shm records");

TEST(FixedString, FromAndView) {
    const auto s = FixedString<10>::from("hello");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->view(), "hello");
    EXPECT_EQ(s->len, 5);
    EXPECT_FALSE(s->empty());
}

TEST(FixedString, RejectsOversizeInsteadOfTruncating) {
    const auto s = FixedString<4>::from("hello");
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error(), StrError::TooLong);
}

TEST(FixedString, ExactCapacityFits) {
    const auto s = FixedString<5>::from("hello");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->view(), "hello");
}

TEST(FixedString, ComparisonAndHash) {
    const auto a = *FixedString<8>::from("aa");
    const auto b = *FixedString<8>::from("bb");
    EXPECT_LT(a, b);
    EXPECT_NE(a, b);

    std::unordered_set<FixedString<8>> set{a, b, a};
    EXPECT_EQ(set.size(), 2U);
}

TEST(FixedString, DefaultIsEmpty) {
    FixedString<8> s{};
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.view(), "");
}

} // namespace
} // namespace raikiri
