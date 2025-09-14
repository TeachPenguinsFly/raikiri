#include <raikiri/venues/bitget/book.hpp>
#include <raikiri/venues/bitget/envelope.hpp>

namespace raikiri::venues::bitget {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// A level is ["63923.9","19.8827"]: price then size, and nothing else. okx
// pads its levels with two more slots; Bitget does not.
[[nodiscard]] std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto text = detail::str_of(value);
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
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
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

// Shared by the books channels and the REST orderbook, whose level arrays are
// identical even though their envelopes are not.
[[nodiscard]] std::expected<void, WireError>
read_book_body(detail::ondemand::object& obj, LevelBuffer& storage, WireBooks& out) {
    const auto bids = read_levels(obj, "bids", storage.bids);
    if (!bids.has_value()) {
        return std::unexpected(bids.error());
    }
    const auto asks = read_levels(obj, "asks", storage.asks);
    if (!asks.has_value()) {
        return std::unexpected(asks.error());
    }
    const auto ts = detail::opt_ms(obj, "ts");
    const auto seq = detail::opt_i64(obj, "seq");
    const auto prev = detail::opt_i64(obj, "pseq");
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

} // namespace

std::expected<WireBooks, WireError> parse_books(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBooks, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        // `action` sits beside `arg`, not inside the data element.
        const auto action = detail::opt_str(*root, "action");
        if (!action.has_value()) {
            return std::unexpected(action.error());
        }
        const bool snapshot = *action == "snapshot";
        return with_first_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBooks, WireError> {
                WireBooks out{};
                out.inst_id = *inst_id;
                out.snapshot = snapshot;
                if (const auto ok = read_book_body(obj, storage, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                return out;
            });
    });
}

std::expected<WireBooks, WireError> parse_rest_orderbook(std::string_view body,
                                                         LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBooks, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBooks out{};
        out.snapshot = true;
        if (const auto ok = read_book_body(*data, storage, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

} // namespace raikiri::venues::bitget
