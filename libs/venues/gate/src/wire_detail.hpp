#pragma once

// Plumbing shared by the gate wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM, so that it
// takes the shape of all of them rather than of whichever was written first.
// What differs is below — Gate's catalogue is a top-level array, so a document
// opens as either shape, and its numeric fields arrive quoted or bare, so
// scalars are read from the raw token with the quotes optional.
//
// simdjson stays confined to this directory; wire.hpp hands out only
// string_view and span.

#include <raikiri/venues/gate/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::gate::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and so needs that many
// readable bytes past the document. A websocket frame is exactly as long as
// Gate made it, so the frame is copied once into a padded scratch buffer that
// lives for the life of the thread, and every view simdjson produces is
// translated back to the caller's buffer (rebase) — nothing handed out points
// into scratch memory the next frame overwrites.
class Session {
public:
    [[nodiscard]] std::expected<ondemand::object, WireError> open_object(std::string_view frame);

    // GET /futures/{settle}/contracts answers with a bare array.
    [[nodiscard]] std::expected<ondemand::array, WireError> open_array(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

private:
    [[nodiscard]] std::expected<void, WireError> load(std::string_view frame);

    simdjson::padded_string scratch_;
    ondemand::parser parser_;
    ondemand::document doc_;
    const char* origin_{nullptr};
    std::size_t origin_size_{0};
};

// One session per thread: rebuilding the structural index allocation per
// frame is the largest avoidable cost simdjson has, and a gateway runs one
// venue connection per thread.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty. Quotes are required, so a bare number is refused.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

// Reads a numeric field whatever its JSON type. Gate quotes prices and leaves
// sizes bare, and its own SDK disagrees with the live endpoint about
// `order_size_min`, so requiring either encoding would break on real data.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(ondemand::object& obj,
                                                                 std::string_view key);

// An absent flag reads false; `in_delisting` is the only one.
[[nodiscard]] std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key);

// Whole part of a decimal token. Gate types several epoch fields as float64 in
// its own SDK and ships the REST book's `current` as 1786394534.46, so the
// fraction is truncated rather than making the field unreadable. Not
// parse_decimal_e9: that scales by 1e9 and an epoch-ms value overflows int64.
// Anything else yields 0, which the mapper reads as "no venue time".
[[nodiscard]] std::uint64_t whole_u64(std::string_view s) noexcept;

// A numeric field as a whole number; absent yields 0.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj,
                                                              std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);

// Every websocket payload hangs off `result`. book_ticker and
// order_book_update put an object there; trades and tickers put an array whose
// elements are the objects.
template <typename Fn>
[[nodiscard]] auto with_result_object(ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<ondemand::object&>())) {
    auto obj = req_object(root, "result");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

// simdjson's On-Demand API throws through its implicit conversions. This file
// never uses those, but "never" is a claim about code other people will edit,
// and an exception escaping into a gateway holding live orders is expensive.
template <typename Fn> [[nodiscard]] auto guarded(Fn&& fn) -> decltype(fn()) {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return std::unexpected(WireError::NotJson);
    }
}

} // namespace raikiri::venues::gate::detail
