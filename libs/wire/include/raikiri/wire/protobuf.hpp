#pragma once

// schema-free reader for mexc spot v3 streams; only length-delimited and varint fields, no
// allocation, no throw

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::wire::pb {

enum class WireType : std::uint8_t {
    Varint = 0,
    Fixed64 = 1,
    Bytes = 2,
    Fixed32 = 5,
};

struct Field {
    std::uint32_t number{0};
    WireType type{WireType::Varint};
    std::uint64_t varint{0}; // Varint, Fixed64 and Fixed32 land here
    std::string_view bytes;  // bytes only; a view into the caller's buffer
};

// forward-only; nullopt at the end and at the first unreadable byte
class Reader {
public:
    explicit constexpr Reader(std::string_view buffer) noexcept : buf_{buffer} {}

    [[nodiscard]] std::optional<Field> next() noexcept;

    [[nodiscard]] constexpr bool done() const noexcept { return pos_ >= buf_.size(); }

private:
    [[nodiscard]] std::optional<std::uint64_t> varint() noexcept;

    std::string_view buf_;
    std::size_t pos_{0};
};

// the first bytes field with this number, as a submessage or a string
[[nodiscard]] std::optional<std::string_view> bytes_field(std::string_view buffer, std::uint32_t number) noexcept;

// The first varint field with this number.
[[nodiscard]] std::optional<std::uint64_t> varint_field(std::string_view buffer, std::uint32_t number) noexcept;

} // namespace raikiri::wire::pb
