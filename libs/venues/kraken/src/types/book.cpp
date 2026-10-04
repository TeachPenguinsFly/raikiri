#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/spot.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>

namespace raikiri::venues::kraken {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireLevel, WireError> read_level(wire::ondemand::value entry) {
    wire::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    const auto price = req_num(obj, "price");
    const auto qty = req_num(obj, "qty");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireLevel{.price = *price, .qty = *qty};
}

std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = wire::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
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

constexpr std::array<std::uint32_t, 256> k_crc_table = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}();

std::uint32_t crc_digits(std::uint32_t crc, std::int64_t value) noexcept {
    std::array<char, 20> digits{};
    const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    for (const char* at = digits.data(); ec == std::errc{} && at != end; ++at) {
        crc = k_crc_table[(crc ^ static_cast<std::uint8_t>(*at)) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}

std::uint32_t crc_levels(std::uint32_t crc, std::span<const book::Level> levels) noexcept {
    for (const auto& level : levels.first(std::min(levels.size(), k_spot_book_depth))) {
        crc = crc_digits(crc, level.price.raw);
        crc = crc_digits(crc, level.qty.raw);
    }
    return crc;
}

} // namespace

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto feed = wire::req_str(*root, "feed");
        const auto product = wire::req_str(*root, "product_id");
        if (!feed.has_value() || !product.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireBook out{};
        out.product_id = *product;
        out.snapshot = *feed == "book_snapshot";
        // seq is contiguous across snapshot and delta
        const auto timestamp = opt_ms(*root, "timestamp");
        const auto seq = opt_num(*root, "seq");
        if (!timestamp.has_value() || !seq.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.timestamp_ms = *timestamp;
        out.seq = wire::digits_to_u64(*seq);

        if (out.snapshot) {
            const auto bids = read_levels(*root, "bids", storage.bids);
            if (!bids.has_value()) {
                return std::unexpected(bids.error());
            }
            const auto asks = read_levels(*root, "asks", storage.asks);
            if (!asks.has_value()) {
                return std::unexpected(asks.error());
            }
            out.bids = {storage.bids.data(), *bids};
            out.asks = {storage.asks.data(), *asks};
            return out;
        }
        const auto side = wire::req_str(*root, "side");
        const auto price = req_num(*root, "price");
        const auto qty = req_num(*root, "qty");
        if (!side.has_value() || !price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        const WireLevel level{.price = *price, .qty = *qty};
        if (*side == "buy") {
            storage.bids[0] = level;
            out.bids = {storage.bids.data(), 1};
        } else if (*side == "sell") {
            storage.asks[0] = level;
            out.asks = {storage.asks.data(), 1};
        } else {
            return std::unexpected(WireError::WrongType);
        }
        return out;
    });
}

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    last_seq_ = 0;
}

bool Book::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto price = exact_steps(wire.price, tick_size_);
        const auto qty = exact_steps(wire.qty, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        const book::Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};

        if (level.qty.raw == 0) {
            // quantity zero deletes the level
            side.remove(level.price);
            continue;
        }
        side.upsert(level);
    }
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        ladders_.clear();
        if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_seq_ = update.seq;
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // a delta before any snapshot has no book
        return BookResult::Ignored;
    }
    if (update.seq != last_seq_ + 1) {
        if (update.seq <= last_seq_) {
            return BookResult::Ignored; // redelivery of a frame already applied
        }
        ++gaps_;
        invalidate();
        return BookResult::GapDetected;
    }

    if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    last_seq_ = update.seq;
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_seq_, ts_event, ts_init);
}

std::expected<WireSpotBook, WireError> parse_spot_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireSpotBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = wire::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        auto data = wire::req_array(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        for (auto entry : *data) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireSpotBook out{};
            out.snapshot = *type == "snapshot";
            const auto symbol = wire::req_str(obj, "symbol");
            const auto checksum = wire::opt_u64(obj, "checksum");
            const auto timestamp = wire::req_str(obj, "timestamp");
            if (!symbol.has_value() || !checksum.has_value() || !timestamp.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto ts = parse_iso8601_ns(*timestamp);
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            const auto bids = read_levels(obj, "bids", storage.bids);
            if (!bids.has_value()) {
                return std::unexpected(bids.error());
            }
            const auto asks = read_levels(obj, "asks", storage.asks);
            if (!asks.has_value()) {
                return std::unexpected(asks.error());
            }
            out.symbol = *symbol;
            out.checksum = static_cast<std::uint32_t>(*checksum);
            out.ts_ns = *ts;
            out.bids = {storage.bids.data(), *bids};
            out.asks = {storage.asks.data(), *asks};
            return out;
        }
        return std::unexpected(WireError::MissingField);
    });
}

std::uint32_t spot_book_checksum(std::span<const book::Level> bids, std::span<const book::Level> asks) {
    return ~crc_levels(crc_levels(0xFFFFFFFFU, asks), bids);
}

SpotBook::SpotBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size, std::size_t depth)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size}, depth_{depth} {}

void SpotBook::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
}

bool SpotBook::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto price = exact_steps(wire.price, tick_size_);
        const auto qty = exact_steps(wire.qty, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        if (*qty == 0) {
            side.remove(PriceTicks::from_raw(*price));
            continue;
        }
        side.upsert({PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)});
    }
    // levels pushed out of the subscribed depth get no removal
    side.truncate(depth_);
    return true;
}

SpotBookResult SpotBook::apply(const WireSpotBook& update) {
    if (update.snapshot) {
        ladders_.clear();
        synced_ = true;
    } else if (!synced_) {
        return SpotBookResult::Ignored;
    }
    if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
        invalidate();
        return SpotBookResult::BadLevel;
    }
    if (spot_book_checksum(bids(), asks()) != update.checksum) {
        ++mismatches_;
        invalidate();
        return SpotBookResult::ChecksumMismatch;
    }
    ++updates_;
    return update.snapshot ? SpotBookResult::Resynced : SpotBookResult::Applied;
}

std::optional<BookSnapshot> SpotBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, updates_, ts_event, ts_init);
}

} // namespace raikiri::venues::kraken
