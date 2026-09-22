#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <optional>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
using wire::session;

// A level is a two-element array of strings, ["79375.3","2.5048"], on the socket
// and in the REST body alike.
std::expected<std::size_t, WireError> read_levels(wire::ondemand::value side,
                                                  std::span<WireLevel> into) {
    wire::ondemand::array array;
    if (const auto ec = side.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    const auto& s = session();
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        WireLevel level{};
        std::size_t field = 0;
        for (auto element : pair) {
            wire::ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto text = wire::str_of(value, s);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            if (field == 0) {
                level.price = *text;
            } else if (field == 1) {
                level.qty = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected(WireError::ShortArray);
        }
        into[count] = level;
        ++count;
    }
    return count;
}

// The venue's own reader: WEEX writes plain decimals on every price and size.
// The exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        using Text = std::expected<std::string_view, WireError>;
        using Number = std::expected<std::uint64_t, WireError>;
        Text event{std::unexpected(WireError::MissingField)};
        Text symbol{std::unexpected(WireError::MissingField)};
        Text disposition{std::unexpected(WireError::MissingField)};
        Number first{0};
        Number last{0};
        Number event_ms{0};
        Number tier{0};
        std::optional<std::size_t> bids;
        std::optional<std::size_t> asks;
        const auto walked = wire::for_each_field(
            *root,
            [&](std::string_view key,
                wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "b" || key == "a") {
                    const bool bid = key == "b";
                    const auto count = read_levels(value, bid ? storage.bids : storage.asks);
                    if (!count.has_value()) {
                        return std::unexpected(count.error());
                    }
                    (bid ? bids : asks) = *count;
                } else if (key == "e") {
                    event = wire::str_of(value);
                } else if (key == "s") {
                    symbol = wire::str_of(value);
                } else if (key == "d") {
                    disposition = wire::str_of(value);
                } else if (key == "U") {
                    first = wire::u64_of(value);
                } else if (key == "u") {
                    last = wire::u64_of(value);
                } else if (key == "E") {
                    event_ms = wire::u64_of(value);
                } else if (key == "l") {
                    tier = wire::u64_of(value);
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        for (auto* text : {&event, &symbol, &disposition}) {
            if (text->has_value() && (*text)->empty()) {
                *text = std::unexpected(WireError::MissingField);
            }
            if (!text->has_value()) {
                return std::unexpected(text->error());
            }
        }

        WireDepth out{};
        out.symbol = *symbol;
        const bool snapshot_event = *event == "depthSnapshot";
        const bool snapshot_field = *disposition == "SNAPSHOT";
        if (snapshot_event != snapshot_field) {
            return std::unexpected(WireError::UnknownMessage);
        }
        if (!snapshot_field && *disposition != "CHANGED") {
            return std::unexpected(WireError::UnknownMessage);
        }
        out.is_snapshot = snapshot_field;

        if (!first.has_value() || !last.has_value() || !event_ms.has_value() || !tier.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (*first == 0 || *last == 0) {
            return std::unexpected(WireError::MissingField); // a frame off the chain
        }
        out.first_id = *first;
        out.last_id = *last;
        out.event_ms = *event_ms;
        out.tier = static_cast<std::uint32_t>(*tier);
        if (!bids.has_value() || !asks.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {
    ladders_.bids().reserve(k_max_levels);
    ladders_.asks().reserve(k_max_levels);
}

void DepthBook::invalidate() noexcept {
    ladders_.clear();
    last_id_ = 0;
    live_ = false;
}

std::optional<DepthBook::Level> DepthBook::convert(const WireLevel& wire) const {
    const auto price = steps_of(wire.price, tick_size_);
    const auto qty = steps_of(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void DepthBook::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        // Quantity "0" is WEEX's delete, and it is how the venue keeps the
        // window at its subscribed width: a level pushed out of the window
        // arrives once more at zero and never again.
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

void DepthBook::apply_wire(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto level = convert(wire);
        if (!level.has_value()) {
            ++dropped_levels_;
            continue;
        }
        upsert(side, *level);
    }
}

BookResult DepthBook::apply(const WireDepth& frame) {
    return frame.is_snapshot ? seed(frame) : advance(frame);
}

BookResult DepthBook::seed(const WireDepth& frame) {
    // A snapshot REPLACES. It arrives when the channel opens and after any
    // resubscribe, and either way what came before it belongs to a chain this
    // frame does not continue.
    ladders_.clear();
    apply_wire(frame.bids, ladders_.bids());
    apply_wire(frame.asks, ladders_.asks());
    last_id_ = frame.last_id;
    live_ = true;
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // One-sided is not a book, and it cannot be published or chained from.
        invalidate();
        return BookResult::Unseeded;
    }
    return BookResult::Applied;
}

BookResult DepthBook::advance(const WireDepth& frame) {
    if (!live_) {
        return BookResult::Unseeded; // silence until the channel's own snapshot
    }
    if (frame.last_id <= last_id_) {
        return BookResult::Stale; // duplicate or reordered; already applied
    }
    // THE BOUNDARY IS SHARED, not successive: `U` equals the PREVIOUS frame's
    // `u`, on 478 of 478 consecutive deltas measured live. Testing for
    // `last_id_ + 1` here would count every valid frame as a gap and leave the
    // book permanently dark.
    if (frame.first_id != last_id_) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    apply_wire(frame.bids, ladders_.bids());
    apply_wire(frame.asks, ladders_.asks());
    last_id_ = frame.last_id;
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        invalidate();
        return BookResult::Gap;
    }
    return BookResult::Applied;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!live_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_id_, ts_event, ts_init);
}

std::optional<Bbo> DepthBook::touch(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!live_) {
        return std::nullopt;
    }
    const auto top = ladders_.touch();
    if (!top.has_value()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = instrument_;
    out.bid = top->first.price;
    out.bid_qty = top->first.qty;
    out.ask = top->second.price;
    out.ask_qty = top->second.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

Reseed due_for_reseed(BookWatch& watch, std::uint64_t now_ns) noexcept {
    if (watch.asked_ns != 0 && now_ns - watch.asked_ns < k_book_silence_ns) {
        return Reseed::None;
    }
    if (watch.gapped) {
        watch.gapped = false;
        watch.digest = 0;
        watch.asked_ns = now_ns;
        return Reseed::Gap;
    }
    const auto last = std::max(watch.seen_ns, watch.asked_ns);
    if (last == 0 || now_ns - last < watch.silence_ns) {
        return Reseed::None;
    }
    watch.asked_ns = now_ns;
    const bool logged = watch.quiet;
    watch.quiet = true;
    return logged ? Reseed::StillSilent : Reseed::Silent;
}

void on_delta(BookWatch& watch, std::uint64_t now_ns) noexcept {
    watch.seen_ns = now_ns;
    watch.silence_ns = k_book_silence_ns;
    watch.digest = 0;
    watch.quiet = false;
}

bool on_reseed_snapshot(BookWatch& watch, std::uint64_t digest) noexcept {
    if (watch.digest == 0) {
        return false;
    }
    const bool missed = digest != watch.digest;
    watch.digest = 0;
    if (missed) {
        watch.silence_ns = k_book_silence_ns;
        watch.quiet = false;
    } else {
        watch.silence_ns = std::min(watch.silence_ns * 2, k_book_silence_cap_ns);
    }
    return missed;
}

std::uint64_t book_digest(const DepthBook& book) noexcept {
    std::uint64_t h = 14'695'981'039'346'656'037ULL;
    const auto mix = [&h](std::int64_t v) {
        h ^= static_cast<std::uint64_t>(v);
        h *= 1'099'511'628'211ULL;
    };
    for (const auto side : {book.bids(), book.asks()}) {
        mix(static_cast<std::int64_t>(side.size()));
        for (const auto& level : side) {
            mix(level.price.raw);
            mix(level.qty.raw);
        }
    }
    return h | 1U;
}

} // namespace raikiri::venues::weex
