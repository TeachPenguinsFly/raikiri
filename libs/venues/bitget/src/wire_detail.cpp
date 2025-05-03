#include "wire_detail.hpp"

namespace raikiri::venues::bitget::detail {

namespace {

// A level is ["63923.9","19.8827"]: price then size, and nothing else. okx
// pads its levels with two more slots; Bitget does not.
[[nodiscard]] std::expected<WireLevel, WireError> read_level(ondemand::value entry) {
    ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    WireLevel level{};
    std::size_t field = 0;
    for (auto element : pair) {
        ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(map_error(ec));
        }
        if (field < 2) {
            const auto text = str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            (field == 0 ? level.price : level.qty) = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

[[nodiscard]] std::expected<std::size_t, WireError>
read_levels(ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

} // namespace

std::expected<void, WireError>
read_book_body(ondemand::object& obj, LevelBuffer& storage, WireBooks& out) {
    const auto bids = read_levels(obj, "bids", storage.bids);
    if (!bids.has_value()) {
        return std::unexpected(bids.error());
    }
    const auto asks = read_levels(obj, "asks", storage.asks);
    if (!asks.has_value()) {
        return std::unexpected(asks.error());
    }
    const auto ts = opt_ms(obj, "ts");
    const auto seq = opt_i64(obj, "seq");
    const auto prev = opt_i64(obj, "pseq");
    if (!ts.has_value() || !seq.has_value() || !prev.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.ts_ms = *ts;
    out.seq_id = *seq;
    out.prev_seq_id = *prev;
    out.bids = {storage.bids.data(), *bids};
    out.asks = {storage.asks.data(), *asks};
    return {};
}

} // namespace raikiri::venues::bitget::detail
