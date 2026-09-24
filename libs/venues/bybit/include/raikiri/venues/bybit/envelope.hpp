#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

enum class TopicKind : std::uint8_t {
    Orderbook,   // orderbook.{depth}.{symbol}
    PublicTrade, // publicTrade.{symbol}
    Tickers,     // tickers.{symbol}
    Order,       // private
    Execution,   // private
    Position,    // private
    Wallet,      // private
    Pong,        // {"op":"pong"} / {"ret_msg":"pong"}
    OpResponse,  // subscribe/auth acknowledgement
    Unknown,
};

struct TopicInfo {
    TopicKind kind{TopicKind::Unknown};
    std::string_view symbol; // empty for private and control frames
};

// exported so subscription keys map to books by the same rule as frames
[[nodiscard]] TopicInfo classify_topic(std::string_view topic) noexcept;

// peeks at "topic"/"op" without consuming the rest of the frame
[[nodiscard]] std::expected<TopicInfo, WireError> parse_topic(std::string_view frame);

// retCode 0 is success
struct WireRestEnvelope {
    std::int64_t ret_code{0};
    std::string_view ret_msg;
    std::string_view result; // raw JSON of "result", parsed by the caller
    std::uint64_t time_ms{0};
};

[[nodiscard]] std::expected<WireRestEnvelope, WireError> parse_envelope(std::string_view body);

// a {"list": [...]} array under a rest result, read into caller storage
template <typename Entry, typename Read>
[[nodiscard]] std::expected<std::span<const Entry>, WireError> read_list(std::string_view frame,
                                                                         std::string_view key,
                                                                         Entry* storage,
                                                                         std::size_t capacity,
                                                                         Read&& read) {
    auto& session = wire::session();
    auto root = session.open_object(frame);
    if (!root.has_value()) {
        return std::unexpected{root.error()};
    }
    auto obj = *root;

    auto data = wire::req_array(obj, key);
    if (!data.has_value()) {
        return std::unexpected{data.error()};
    }
    std::size_t count = 0;
    for (auto item : *data) {
        if (count == capacity) {
            return std::unexpected{WireError::TooManyEntries};
        }
        wire::ondemand::object entry_obj;
        if (item.get_object().get(entry_obj) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        Entry entry{};
        if (auto ok = read(entry_obj, entry); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        storage[count++] = entry;
    }
    return std::span<const Entry>{storage, count};
}

} // namespace raikiri::venues::bybit
