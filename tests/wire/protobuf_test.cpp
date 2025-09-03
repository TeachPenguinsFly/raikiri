#include <raikiri/wire/protobuf.hpp>

#include <gtest/gtest.h>

#include <string>

namespace raikiri::wire::pb {
namespace {

// Builds the encoding by hand so the tests state the bytes they mean rather
// than trusting the decoder to describe itself.
std::string varint(std::uint64_t value) {
    std::string out;
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        out.push_back(static_cast<char>(byte));
    } while (value != 0);
    return out;
}

std::string key(std::uint32_t number, WireType type) {
    return varint((static_cast<std::uint64_t>(number) << 3U) | static_cast<std::uint64_t>(type));
}

std::string bytes(std::uint32_t number, std::string_view text) {
    return key(number, WireType::Bytes) + varint(text.size()) + std::string{text};
}

std::string number(std::uint32_t field, std::uint64_t value) {
    return key(field, WireType::Varint) + varint(value);
}

TEST(Protobuf, ReadsTheThreeWireTypesTheStreamsUse) {
    const auto message = bytes(1, "BTCUSDT") + number(6, 1'787'837'770'652ULL) +
                         bytes(315,
                               std::string{"\x0a\x02"
                                           "hi",
                                           4});

    Reader reader{message};
    auto first = reader.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->number, 1U);
    EXPECT_EQ(first->type, WireType::Bytes);
    EXPECT_EQ(first->bytes, "BTCUSDT");

    auto second = reader.next();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->number, 6U);
    EXPECT_EQ(second->type, WireType::Varint);
    EXPECT_EQ(second->varint, 1'787'837'770'652ULL);

    auto third = reader.next();
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->number, 315U) << "field numbers past 15 take two key bytes";
    EXPECT_EQ(third->bytes.size(), 4U);

    EXPECT_FALSE(reader.next().has_value());
    EXPECT_TRUE(reader.done());
}

TEST(Protobuf, AFieldIsFoundByNumberAndAnUnknownOneIsSkipped) {
    // The whole reason for reading by number: a venue that adds a field must
    // not break a decoder that never asked for it.
    const auto message = number(2, 7) + bytes(1, "first") + number(99, 5) + bytes(3, "third");

    EXPECT_EQ(bytes_field(message, 1), "first");
    EXPECT_EQ(bytes_field(message, 3), "third");
    EXPECT_EQ(varint_field(message, 2), 7U);
    EXPECT_EQ(varint_field(message, 99), 5U);
    EXPECT_FALSE(bytes_field(message, 4).has_value());
    // Right number, wrong wire type: not an answer.
    EXPECT_FALSE(bytes_field(message, 2).has_value());
    EXPECT_FALSE(varint_field(message, 1).has_value());
}

TEST(Protobuf, ATruncatedMessageStopsRatherThanReadingPastItself) {
    const auto whole = bytes(1, "BTCUSDT") + number(6, 42);
    for (std::size_t cut = 1; cut < whole.size(); ++cut) {
        const std::string_view partial{whole.data(), cut};
        Reader reader{partial};
        // Whatever it manages to read, it must terminate and must never claim
        // a field whose bytes are not all present.
        std::size_t fields = 0;
        while (const auto field = reader.next()) {
            ASSERT_LE(field->bytes.size(), partial.size());
            ASSERT_LT(++fields, 8U) << "cut at " << cut;
        }
        SUCCEED();
    }
}

TEST(Protobuf, GarbageEndsIterationInsteadOfInventingFields) {
    // Wire types 3, 4, 6 and 7 are not readable: 3 and 4 were removed from the
    // language and the rest were never assigned.
    for (const std::uint64_t type : {3ULL, 4ULL, 6ULL, 7ULL}) {
        const auto message = varint((1ULL << 3U) | type) + std::string(8, '\x01');
        Reader reader{message};
        EXPECT_FALSE(reader.next().has_value()) << "wire type " << type;
    }
    // Field number zero cannot be assigned, so a buffer that starts with one
    // is not a message.
    const auto zero = varint(0) + std::string(4, '\x01');
    Reader zero_reader{zero};
    EXPECT_FALSE(zero_reader.next().has_value());

    Reader empty{std::string_view{}};
    EXPECT_TRUE(empty.done());
    EXPECT_FALSE(empty.next().has_value());
}

TEST(Protobuf, AnOverlongVarintIsRefused) {
    // Eleven continuation bytes cannot be a u64, and accepting them would make
    // the decoder disagree with every other reader about where the field ends.
    const auto message = key(1, WireType::Varint) + std::string(11, '\xFF');
    Reader reader{message};
    EXPECT_FALSE(reader.next().has_value());
}

} // namespace
} // namespace raikiri::wire::pb
