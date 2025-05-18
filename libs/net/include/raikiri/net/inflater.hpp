#pragma once

// Gzip frame decompression for venues that compress their market data.
//
// This sits in net rather than in a venue because it is a property of the
// transport, not of any exchange: libs/net hands on_message the bytes of a
// binary websocket message unchanged — it never inspects the opcode and
// decompresses nothing — so whoever knows the frames are compressed has to
// inflate them. BingX and HTX both do, identically.

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace raikiri::net {

enum class InflateError : std::uint8_t {
    NotGzip,   // no 1f 8b magic, or a compression method that is not deflate
    Truncated, // the gzip header runs past the end of the frame
    TooLarge,  // expands past the ceiling: a decompression bomb, not a quote
    Corrupt,   // the deflate stream itself does not decode
};

[[nodiscard]] std::string_view to_string(InflateError err) noexcept;

// Gzip -> the JSON the wire parsers read. The returned view points into this
// object's buffer and lives only until the next call — on_message's own
// contract exactly.
class Inflater {
public:
    Inflater();
    ~Inflater();
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;

    [[nodiscard]] std::expected<std::string_view, InflateError> inflate(std::string_view frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
