#pragma once

// fixed-buffer json writer: no allocation, no throw, prices rendered from fixed-point
// overflow is sticky; check ok() once at the end

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

    // same, but unquoted
    JsonWriter& decimal_raw(std::string_view key, FixedE9 value) noexcept;

    // pre-rendered fragment, already valid json
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
