#pragma once

// gzip frame decompression for bingx and htx; net passes binary messages through untouched

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

// the returned view is valid until the next call
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
