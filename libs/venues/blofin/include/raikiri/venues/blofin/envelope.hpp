#pragma once

#include <raikiri/venues/blofin/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::blofin {

enum class EventKind : std::uint8_t {
    Books,
    Trades,
    Tickers,
    FundingRate,
    SubscribeAck,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view inst_id;
};

// the keepalive answer is the bare text "pong" and fails here as NotJson
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
    // recovered from the frame the venue echoes inside `msg`; empty when it echoes none
    std::string_view inst_id;
    std::size_t args{0};
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError> read_data(std::string_view body, std::span<Entry> into, Read&& read) {
    return wire::guarded([&]() -> std::expected<std::span<const Entry>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto element : *array) {
            if (count == into.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            Entry entry{};
            if (const auto ok = read(obj, entry); !ok.has_value()) {
                return std::unexpected(ok.error());
            }
            into[count] = entry;
            ++count;
        }
        return std::span<const Entry>{into.data(), count};
    });
}

} // namespace raikiri::venues::blofin
