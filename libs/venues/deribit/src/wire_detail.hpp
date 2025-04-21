#pragma once

// Plumbing shared by the deribit wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM, so it takes the
// shape of all of them rather than of whichever was written first. What
// already differs is the number reader — Deribit puts prices, sizes and
// timestamps in JSON NUMBERS where OKX puts digit strings, so num_of sits
// beside str_of and both hand back raw tokens.
//
// simdjson stays confined to this directory; wire.hpp hands out only
// string_view and span.

#include <raikiri/venues/deribit/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::deribit::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and needs that many readable
// bytes past the document, so a frame is copied once into a thread-lifetime
// padded buffer and every view simdjson produces is translated back to the
// caller's buffer (rebase) — nothing handed out points into scratch memory the
// next frame overwrites.
class Session {
public:
    [[nodiscard]] std::expected<ondemand::object, WireError> open_object(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

private:
    simdjson::padded_string scratch_;
    ondemand::parser parser_;
    ondemand::document doc_;
    const char* origin_{nullptr};
    std::size_t origin_size_{0};
};

// One per thread: rebuilding the structural index allocation per frame is the
// largest avoidable cost simdjson has.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

// Reads a JSON number as its raw token, likewise over the caller's buffer, in
// whatever form Deribit chose ("0.5", "2.7e4"); decoding is parse_number_e9's
// job at the mapping edge.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(ondemand::object& obj,
                                                                 std::string_view key);

// change_id and trade_seq are plain JSON integers; a missing one reads 0.
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);

[[nodiscard]] std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key);

// Epoch milliseconds. These must NOT go through parse_number_e9: it scales by
// 1e9 and an epoch-ms value overflows int64 on the way. std::from_chars over
// the raw token is the whole conversion; anything else yields 0, which the
// mapper reads as "venue gave us no time" and replaces with ingest time.
[[nodiscard]] std::uint64_t token_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
                                                             std::string_view key);

// Every push wraps its payload in params.data.
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);

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

} // namespace raikiri::venues::deribit::detail
