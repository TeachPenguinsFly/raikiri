#pragma once

// Fixed-buffer JSON writer for outbound venue requests.
//
// Order payloads are tiny and their shapes are fixed, so a general JSON
// library is the wrong tool on this path: it allocates, it may throw, and it
// formats numbers through code that knows nothing about our fixed-point
// invariants. This writer takes the caller's buffer, never allocates, never
// throws, and renders prices straight from int64 fixed-point without ever
// touching a floating-point instruction — the one property that matters,
// since a double round-trip is exactly how a price becomes off by a tick.
//
// Overflow is sticky, not fatal: the writer stops appending and reports
// ok() == false, so the caller checks once at the end rather than after
// every field.

#include <raikiri/common/fixed_point.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::net {

class JsonWriter {
public:
    explicit JsonWriter(std::span<char> buffer) noexcept : buf_{buffer} {}

    JsonWriter& obj_begin() noexcept;
    JsonWriter& obj_end() noexcept;
    JsonWriter& arr_begin() noexcept;
    JsonWriter& arr_end() noexcept;

    JsonWriter& str(std::string_view key, std::string_view value) noexcept;
    JsonWriter& num(std::string_view key, std::int64_t value) noexcept;
    JsonWriter& boolean(std::string_view key, bool value) noexcept;

    JsonWriter& decimal(std::string_view key, FixedE9 value) noexcept;

    // Same, but rendered unquoted for the venues that want a JSON number.
    JsonWriter& decimal_raw(std::string_view key, FixedE9 value) noexcept;

    // Pre-rendered fragment (already valid JSON), for cached constants.
    JsonWriter& raw(std::string_view key, std::string_view json) noexcept;

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::string_view view() const noexcept {
        return overflow_ ? std::string_view{} : std::string_view{buf_.data(), len_};
    }

    void reset() noexcept {
        len_ = 0;
        overflow_ = false;
        need_comma_ = false;
    }

private:
    void put(char c) noexcept;
    void put(std::string_view s) noexcept;
    void put_escaped(std::string_view s) noexcept;
    void separator() noexcept;
    void put_key(std::string_view key) noexcept;

    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
    bool need_comma_{false};
};

} // namespace raikiri::net
