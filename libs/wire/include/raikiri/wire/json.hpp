#pragma once

// simdjson stays here and in venue src/; venue headers hand out string_view and span

#include <raikiri/wire/error.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>
#include <utility>

namespace raikiri::wire {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] Error map_error(simdjson::error_code ec) noexcept;

// on-demand needs SIMDJSON_PADDING readable bytes past the document, so frames are copied
// into a padded per-thread buffer and every view is rebased onto the caller's buffer
class Session {
public:
    [[nodiscard]] std::expected<ondemand::object, Error> open_object(std::string_view frame);

    [[nodiscard]] std::expected<ondemand::array, Error> open_array(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

    // false once simdjson abandoned the document; touching it after that is undefined
    [[nodiscard]] bool alive() const noexcept { return doc_.is_alive(); }

    [[nodiscard]] const char* location() const noexcept {
        const char* at = nullptr;
        return doc_.current_location().get(at) == simdjson::SUCCESS ? at : nullptr;
    }

private:
    [[nodiscard]] std::expected<void, Error> start(std::string_view frame);

    simdjson::padded_string scratch_;
    ondemand::parser parser_;
    mutable ondemand::document doc_;
    const char* origin_{nullptr};
    std::size_t origin_size_{0};
};

// one session per thread to reuse the structural index
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty.
[[nodiscard]] std::expected<std::string_view, Error> str_of(ondemand::value value,
                                                            const Session& s = session());

// The token as written, trimmed, whatever its type.
[[nodiscard]] std::expected<std::string_view, Error> raw_of(ondemand::value value,
                                                            const Session& s = session());

[[nodiscard]] std::expected<std::uint64_t, Error> u64_of(ondemand::value value);
[[nodiscard]] std::expected<std::int64_t, Error> i64_of(ondemand::value value);
[[nodiscard]] std::expected<bool, Error> bool_of(ondemand::value value);

// every field once, in wire order; fn may return void or std::expected<void, Error>
template <typename Fn>
[[nodiscard]] std::expected<void, Error> for_each_field(ondemand::object& obj, Fn&& fn) {
    for (auto field : obj) {
        std::string_view key;
        if (const auto ec = field.escaped_key().get(key); ec != simdjson::SUCCESS) {
            return std::unexpected(map_error(ec));
        }
        ondemand::value value;
        if (const auto ec = field.value().get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(map_error(ec));
        }
        if constexpr (std::is_void_v<
                          std::invoke_result_t<Fn&, std::string_view, ondemand::value&>>) {
            fn(key, value);
        } else if (auto done = fn(key, value); !done.has_value()) {
            return std::unexpected(done.error());
        }
    }
    return {};
}

// find_field_unordered with every step of the wraparound pass checked, safe on a malformed frame
[[nodiscard]] simdjson::simdjson_result<ondemand::value> find(ondemand::object& obj,
                                                              std::string_view key);

[[nodiscard]] std::expected<std::string_view, Error> opt_str(ondemand::object& obj,
                                                             std::string_view key);
[[nodiscard]] std::expected<std::string_view, Error> req_str(ondemand::object& obj,
                                                             std::string_view key);

// for ids and timestamps sent as json numbers
[[nodiscard]] std::expected<std::uint64_t, Error> opt_u64(ondemand::object& obj,
                                                          std::string_view key);
[[nodiscard]] std::expected<std::int64_t, Error> opt_i64(ondemand::object& obj,
                                                         std::string_view key);
[[nodiscard]] std::expected<bool, Error> opt_bool(ondemand::object& obj, std::string_view key);

// raw json text of a field, for a payload handed to a second parser
[[nodiscard]] std::expected<std::string_view, Error> opt_raw(ondemand::object& obj,
                                                             std::string_view key);

[[nodiscard]] std::expected<ondemand::array, Error> req_array(ondemand::object& obj,
                                                              std::string_view key);
[[nodiscard]] std::expected<ondemand::object, Error> req_object(ondemand::object& obj,
                                                                std::string_view key);

// on-demand throws through its implicit conversions; this keeps an exception out of a gateway
template <typename Fn> [[nodiscard]] auto guarded(Fn&& fn) -> decltype(fn()) {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return std::unexpected(Error::NotJson);
    }
}

[[nodiscard]] std::expected<std::uint64_t, Error> opt_ms(ondemand::object& obj,
                                                         std::string_view key);

[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

[[nodiscard]] bool has_field(ondemand::object& obj, std::string_view key);

} // namespace raikiri::wire
