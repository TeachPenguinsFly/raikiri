#pragma once

// simdjson plumbing shared by every venue adapter. Sixteen adapters carried a
// near-identical copy until the drift between them turned out to be accident
// rather than need.
//
// simdjson is visible here and in venue src/ files, nowhere else: venue wire
// headers hand out std::string_view and std::span only.

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

// On-Demand reads in SIMDJSON_PADDING-byte blocks and so needs that many
// readable bytes past the document. A websocket frame is exactly as long as
// binance made it, so the frame is copied once into a padded scratch buffer
// that lives for the life of the thread, and every view simdjson produces is
// translated back to the caller's buffer (rebase) — nothing handed out points
// into scratch memory the next frame overwrites.
class Session {
public:
    [[nodiscard]] std::expected<ondemand::object, Error> open_object(std::string_view frame);

    [[nodiscard]] std::expected<ondemand::array, Error> open_array(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

    // False once simdjson has abandoned the document on a malformed frame.
    // Touching it after that is undefined: a lookup rewinds and rescans bytes
    // it never validated, with the parser pointer already cleared.
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

// One session per thread: rebuilding the structural index allocation per
// frame is the largest avoidable cost simdjson has, and a gateway runs one
// venue connection per thread.
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

// Every field of obj once, in wire order, as fn(key, value). A lookup by name
// for a key that is absent or behind the cursor rescans the object, arrays and
// all. fn may return void or std::expected<void, Error>; an error stops the walk.
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

// find_field_unordered with its second pass, from the top of the object back
// to where the search began, redone with every step checked. simdjson's own
// trusts those bytes were validated on the way past, and a malformed frame
// read partway breaks that: an assert in debug, UB in release.
[[nodiscard]] simdjson::simdjson_result<ondemand::value> find(ondemand::object& obj,
                                                              std::string_view key);

[[nodiscard]] std::expected<std::string_view, Error> opt_str(ondemand::object& obj,
                                                             std::string_view key);
[[nodiscard]] std::expected<std::string_view, Error> req_str(ondemand::object& obj,
                                                             std::string_view key);

// Binance timestamps and ids are JSON numbers, not digit strings — this is
// the one place the difference from bybit shows up in the helpers.
[[nodiscard]] std::expected<std::uint64_t, Error> opt_u64(ondemand::object& obj,
                                                          std::string_view key);
[[nodiscard]] std::expected<std::int64_t, Error> opt_i64(ondemand::object& obj,
                                                         std::string_view key);
[[nodiscard]] std::expected<bool, Error> opt_bool(ondemand::object& obj, std::string_view key);

// Raw JSON text of a field, whatever its type — the stream wrapper's "data"
// is handed to a second parser rather than decoded here.
[[nodiscard]] std::expected<std::string_view, Error> opt_raw(ondemand::object& obj,
                                                             std::string_view key);

[[nodiscard]] std::expected<ondemand::array, Error> req_array(ondemand::object& obj,
                                                              std::string_view key);
[[nodiscard]] std::expected<ondemand::object, Error> req_object(ondemand::object& obj,
                                                                std::string_view key);

// simdjson's On-Demand API throws through its implicit conversions. This file
// never uses those, but "never" is a claim about code other people will edit,
// and an exception escaping into a gateway holding live orders is expensive.
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
