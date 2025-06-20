#pragma once

// Reading protobuf off the wire, without a schema and without a runtime.
//
// One venue speaks it — MEXC's spot v3 streams, where the JSON endpoint is
// refused outright — and every field it sends is a length-delimited string or
// a varint. That is two wire types out of six, so a full protobuf runtime
// would be a dependency bought for a decoder that fits on a page.
//
// SCHEMA-FREE BY DESIGN. A .proto would have to be kept in step with a venue
// that ships changes without telling anyone; asking for a field number instead
// means an unknown field is skipped rather than fatal, and a field that moves
// is a parse that finds nothing rather than one that silently reads the wrong
// bytes. Callers name the numbers where they use them, next to the capture
// that proves them.
//
// Nothing here allocates and nothing here throws: a truncated or malformed
// buffer ends iteration, and the caller sees the fields that were whole.

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
    std::uint64_t varint{0};      // Varint, Fixed64 and Fixed32 land here
    std::string_view bytes;       // Bytes only; a view INTO the caller's buffer
};

// Forward-only cursor over one message. `next` returns nullopt at the end and
// at the first byte it cannot make sense of — the two are deliberately the
// same answer, because a message that stops early has no more fields either
// way and guessing past the damage is how a decoder invents data.
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

// The first Bytes field with this number, as a submessage or a string. Reading
// by number rather than by position is what makes an unknown field harmless.
[[nodiscard]] std::optional<std::string_view> bytes_field(std::string_view buffer,
                                                          std::uint32_t number) noexcept;

// The first varint field with this number.
[[nodiscard]] std::optional<std::uint64_t> varint_field(std::string_view buffer,
                                                        std::uint32_t number) noexcept;

} // namespace raikiri::wire::pb
