#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::kucoin {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// "62234.6,buy,1" -> price, side, size. A partly-formed change would apply a
// real price at an invented size, so anything short of three fields fails.
std::expected<void, WireError> split_change(std::string_view change, WireBookChange& out) {
    const auto first = change.find(',');
    if (first == std::string_view::npos) {
        return std::unexpected(WireError::WrongType);
    }
    const auto second = change.find(',', first + 1);
    if (second == std::string_view::npos) {
        return std::unexpected(WireError::WrongType);
    }
    out.price = change.substr(0, first);
    const auto side = change.substr(first + 1, second - first - 1);
    if (side != "buy" && side != "sell") {
        return std::unexpected(WireError::WrongType);
    }
    out.bid = side == "buy";
    out.qty = change.substr(second + 1);
    if (out.price.empty() || out.qty.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

// The spot book spells a level as a two-element ARRAY, ["79497","0.01068"],
// where the futures stream packs one into a comma-joined string. Same numbers,
// nothing shared.
std::expected<std::size_t, WireError>
read_pair_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        std::string_view fields[2];
        std::size_t seen = 0;
        for (auto field : pair) {
            if (seen == 2) {
                return std::unexpected(WireError::WrongType);
            }
            std::string_view text;
            if (const auto ec = field.value().raw_json().get(text); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                text = text.substr(1, text.size() - 2);
            }
            fields[seen++] = session().rebase(text);
        }
        if (seen != 2) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = fields[0], .qty = fields[1]};
        ++count;
    }
    return count;
}

// A level is [price, size] with both members written as JSON numbers.
std::expected<void, WireError>
read_pair(detail::ondemand::value entry, std::string_view& price, std::string_view& qty) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto raw = raw_of(value);
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            (field == 0 ? price : qty) = *raw;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<std::size_t, WireError>
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
        if (const auto ok = read_pair(value, into[count].price, into[count].qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        ++count;
    }
    return count;
}

// The venue's own reader; the exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame, MarketType market) {
    const bool spot = market == MarketType::Spot;
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return with_data(*root,
                         [&](detail::ondemand::object& obj) -> std::expected<WireBbo, WireError> {
                             WireBbo out{};
                             out.symbol = *symbol;
                             // Futures: bestBidPrice/bestBidSize and a NANOSECOND `ts`.
                             // Spot:    bestBid/bestBidSize and a MILLISECOND `time`, with
                             // the sequence as a digit string rather than a number.
                             const auto bid = req_decimal(obj, spot ? "bestBid" : "bestBidPrice");
                             const auto bid_qty = req_decimal(obj, "bestBidSize");
                             const auto ask = req_decimal(obj, spot ? "bestAsk" : "bestAskPrice");
                             const auto ask_qty = req_decimal(obj, "bestAskSize");
                             if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() ||
                                 !ask_qty.has_value()) {
                                 return std::unexpected(WireError::MissingField);
                             }
                             out.bid_price = *bid;
                             out.bid_qty = *bid_qty;
                             out.ask_price = *ask;
                             out.ask_qty = *ask_qty;
                             if (spot) {
                                 const auto ms = opt_u64(obj, "time");
                                 if (!ms.has_value()) {
                                     return std::unexpected(WireError::WrongType);
                                 }
                                 out.ts_ns = *ms * 1'000'000ULL;
                                 out.sequence = 0; // a digit string here; nothing reads it
                                 return out;
                             }
                             const auto ts = opt_u64(obj, "ts");
                             const auto sequence = opt_i64(obj, "sequence");
                             if (!ts.has_value() || !sequence.has_value()) {
                                 return std::unexpected(WireError::WrongType);
                             }
                             out.ts_ns = *ts;
                             out.sequence = *sequence;
                             return out;
                         });
    });
}

std::expected<WireBookChange, WireError> parse_book_change(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBookChange, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBookChange, WireError> {
                WireBookChange out{};
                out.symbol = *symbol;
                const auto change = detail::req_str(obj, "change");
                if (!change.has_value()) {
                    return std::unexpected(change.error());
                }
                if (const auto ok = split_change(*change, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                const auto ts = opt_u64(obj, "timestamp");
                const auto sequence = opt_i64(obj, "sequence");
                if (!ts.has_value() || !sequence.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts_ms = *ts;
                out.sequence = *sequence;
                return out;
            });
    });
}

// /spotMarket/level2Depth50: a whole book, levels as ["price","size"] pairs.
// Nothing to seed and nothing to chain — the opposite of the futures level2
// stream, which sends one packed level at a time and cannot start on its own.
std::expected<WireBookSnapshot, WireError> parse_book_snapshot(std::string_view frame,
                                                               LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBookSnapshot, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return with_data(
            *root,
            [&](detail::ondemand::object& obj) -> std::expected<WireBookSnapshot, WireError> {
                WireBookSnapshot out{};
                out.symbol = *symbol;
                const auto asks = read_pair_levels(obj, "asks", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto bids = read_pair_levels(obj, "bids", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                out.asks = {storage.asks.data(), *asks};
                out.bids = {storage.bids.data(), *bids};
                out.ts_ms = opt_u64(obj, "timestamp").value_or(0);
                return out;
            });
    });
}

std::expected<WireUtaBook, WireError> parse_uta_book(std::string_view frame,
                                                     LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireUtaBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // `t` separates the opening whole book from the deltas that follow it.
        // Anything else on this channel is not a book frame.
        const auto kind = detail::req_str(*root, "t");
        if (!kind.has_value()) {
            return std::unexpected(kind.error());
        }
        const bool snapshot = *kind == "snapshot";
        if (!snapshot && *kind != "delta") {
            return std::unexpected(WireError::WrongType);
        }
        auto data = detail::req_object(*root, "d");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireUtaBook out{};
        out.is_snapshot = snapshot;
        const auto symbol = detail::req_str(*data, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        // COMPACT KEYS, and every one of them is required. A frame missing its
        // span cannot be chained, and one missing a side cannot be applied —
        // there is no "absent means unchanged" here, an untouched side arrives
        // as an empty array.
        //
        // PRESENCE IS CHECKED SEPARATELY because opt_i64 answers an absent
        // field with zero rather than an error, and zero is a sequence a
        // reader would happily chain from.
        if (!detail::has_field(*data, "O") || !detail::has_field(*data, "C")) {
            return std::unexpected(WireError::MissingField);
        }
        const auto start = detail::opt_i64(*data, "O");
        const auto end = detail::opt_i64(*data, "C");
        if (!start.has_value() || !end.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.seq_start = *start;
        out.seq_end = *end;
        const auto asks = read_pair_levels(*data, "a", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto bids = read_pair_levels(*data, "b", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        out.asks = {storage.asks.data(), *asks};
        out.bids = {storage.bids.data(), *bids};
        // `M` is the matching engine's own instant in NANOSECONDS, not the
        // milliseconds every classic KuCoin frame carries.
        out.ts_ns = detail::opt_u64(*data, "M").value_or(0);
        return out;
    });
}

std::expected<WireOrderBook, WireError> parse_level2_snapshot(std::string_view body,
                                                              LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderBook, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireOrderBook, WireError> {
                WireOrderBook out{};
                const auto symbol = detail::req_str(obj, "symbol");
                const auto sequence = opt_i64(obj, "sequence");
                const auto ts = opt_u64(obj, "ts");
                if (!symbol.has_value() || !sequence.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                // Without a sequence the levels cannot be placed in the change
                // chain, and a book that cannot be continued is not a seed.
                if (*sequence <= 0) {
                    return std::unexpected(WireError::MissingField);
                }
                out.symbol = *symbol;
                out.sequence = *sequence;
                out.ts_ns = *ts;
                const auto bids = read_levels(obj, "bids", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(obj, "asks", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                out.bids = {storage.bids.data(), *bids};
                out.asks = {storage.asks.data(), *asks};
                return out;
            });
    });
}

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

bool DepthBook::wants_seed(std::uint64_t now_ns) const noexcept {
    return state_ == State::Cold && !seed_in_flight_ && now_ns >= seed_retry_at_ns_;
}

void DepthBook::invalidate() noexcept {
    ladders_.clear();
    pending_.clear();
    held_.clear();
    held_sequence_ = 0;
    last_sequence_ = 0;
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    state_ = State::Cold;
    seed_in_flight_ = false;
    // The wait is NOT cleared: a book invalidated by a gap is exactly the one
    // whose seed is most likely to be refused next, and clearing it here would
    // put the retry back on the frame rate.
}

std::optional<DepthBook::Level> DepthBook::convert(std::string_view price,
                                                   std::string_view qty) const {
    const auto ticks = steps_of(price, tick_size_);
    const auto lots = steps_of(qty, lot_size_);
    if (!ticks.has_value() || !lots.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*ticks), .qty = QtyLots::from_raw(*lots)};
}

void DepthBook::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        // Size zero is KuCoin's delete: 1914 of 6804 live frames carry one.
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

void DepthBook::place(const Level& level, bool bid) {
    upsert(bid ? ladders_.bids() : ladders_.asks(), level);
}

BookResult DepthBook::apply(const WireBookChange& change) {
    const auto level = convert(change.price, change.qty);
    if (!level.has_value()) {
        // A level we cannot convert is not one we may skip: from here the book
        // would disagree with KuCoin's permanently, and invisibly.
        invalidate();
        return BookResult::BadLevel;
    }
    switch (state_) {
    case State::Cold:
        return buffer(change.sequence, *level, change.bid);
    case State::Bridging:
        return bridge(change.sequence, *level, change.bid);
    case State::Live:
        return advance(change.sequence, *level, change.bid);
    }
    return BookResult::Waiting;
}

BookResult DepthBook::buffer(std::int64_t sequence, const Level& level, bool bid) {
    if (!pending_.empty() && sequence != pending_.back().sequence + 1) {
        pending_.clear(); // the chain broke before the seed landed
    }
    if (pending_.size() == k_max_buffered_changes) {
        pending_.clear();
    }
    pending_.push_back(Buffered{.sequence = sequence, .level = level, .bid = bid});
    return BookResult::Buffered;
}

BookResult DepthBook::bridge(std::int64_t sequence, const Level& level, bool bid) {
    const auto wanted = held_sequence_ + 1;
    if (sequence < wanted) {
        return BookResult::Stale; // this change is already in the seed
    }
    if (sequence > wanted) {
        // The chain is contiguous, so nothing will ever fill what is missing
        // between the seed and here.
        discard_held();
        return BookResult::Reseed;
    }
    install_held();
    place(level, bid);
    last_sequence_ = sequence;
    return settle();
}

BookResult DepthBook::advance(std::int64_t sequence, const Level& level, bool bid) {
    if (sequence <= last_sequence_) {
        return BookResult::Stale; // duplicate or reordered; already applied
    }
    if (sequence != last_sequence_ + 1) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    place(level, bid);
    last_sequence_ = sequence;
    return settle();
}

BookResult DepthBook::seed(const WireOrderBook& wire) {
    seed_in_flight_ = false;
    if (state_ == State::Live) {
        return BookResult::Stale; // a seed outlived the book it was meant for
    }
    held_.clear();
    const auto take = [this](std::span<const WireLevel> src, book::Ladder& into) {
        into.reserve(src.size());
        for (const auto& entry : src) {
            const auto level = convert(entry.price, entry.qty);
            if (!level.has_value()) {
                return false;
            }
            into.upsert(*level);
        }
        return true;
    };
    // A seed with one level we cannot read is not a seed we can chain onto,
    // and a hole in it would never heal — the stream only sends changes.
    if (!take(wire.bids, held_.bids()) || !take(wire.asks, held_.asks()) || held_.bids().empty() ||
        held_.asks().empty()) {
        discard_held();
        return BookResult::Reseed;
    }
    held_sequence_ = wire.sequence;
    band_low_ = held_.bids().levels().back().price;
    band_high_ = held_.asks().levels().back().price;
    state_ = State::Bridging;
    return replay();
}

BookResult DepthBook::replay() {
    const auto wanted = held_sequence_ + 1;
    std::size_t i = 0;
    while (i < pending_.size() && pending_[i].sequence < wanted) {
        ++i;
    }
    if (i == pending_.size()) {
        pending_.clear();
        return BookResult::Waiting; // the seed is newer than anything buffered
    }
    if (pending_[i].sequence > wanted) {
        discard_held();
        return BookResult::Reseed;
    }

    install_held();
    // buffer() already enforced contiguity across pending_, so the replay only
    // has to find where the seed sits inside it.
    for (; i < pending_.size(); ++i) {
        place(pending_[i].level, pending_[i].bid);
        last_sequence_ = pending_[i].sequence;
    }
    pending_.clear();
    return settle();
}

BookResult DepthBook::settle() {
    // Past the band the seed vouched for, a level is one no snapshot ever
    // confirmed; carrying it would make the book look deeper than it is known
    // to be.
    ladders_.bids().drop_beyond(band_low_);
    ladders_.asks().drop_beyond(band_high_);
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // The price walked clean out of the band, so what is left is no longer
        // the top of the venue's book.
        invalidate();
        return BookResult::Reseed;
    }
    return BookResult::Applied;
}

void DepthBook::install_held() {
    ladders_ = std::move(held_);
    held_.clear();
    last_sequence_ = held_sequence_;
    state_ = State::Live;
}

void DepthBook::discard_held() {
    held_.clear();
    held_sequence_ = 0;
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    state_ = State::Cold;
    ++reseeds_;
}

UtaBook::UtaBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

bool UtaBook::load(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& level : levels) {
        const auto ticks = steps_of(level.price, tick_size_);
        const auto lots = steps_of(level.qty, lot_size_);
        if (!ticks.has_value() || !lots.has_value()) {
            // A level we cannot convert is not one we may skip: from here the
            // book would disagree with KuCoin's permanently, and invisibly.
            return false;
        }
        const auto price = PriceTicks::from_raw(*ticks);
        if (*lots == 0) {
            side.remove(price); // size zero is KuCoin's delete, on both feeds
            continue;
        }
        side.upsert(Level{.price = price, .qty = QtyLots::from_raw(*lots)});
    }
    return true;
}

BookResult UtaBook::apply(const WireUtaBook& frame) {
    if (frame.is_snapshot) {
        // THE WHOLE BOOK, so whatever was there is replaced rather than merged
        // into. A snapshot arriving mid-stream is the venue restating the book
        // after its own break, and merging would keep levels it has dropped.
        ladders_.clear();
        if (!load(frame.asks, ladders_.asks()) || !load(frame.bids, ladders_.bids())) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_end_ = frame.seq_end;
        live_ = true;
        return BookResult::Applied;
    }
    if (!live_) {
        // Deltas before the opening snapshot refer to a book we do not have.
        return BookResult::Waiting;
    }
    if (frame.seq_end <= last_end_) {
        return BookResult::Stale; // a replay, already inside the book
    }
    if (frame.seq_start > last_end_ + 1) {
        // THE CHAIN BROKE. Unlike classic, there is no seed to fetch: the
        // venue resends a snapshot on this same subscription, so the book goes
        // silent and waits for it rather than asking for anything.
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    if (!load(frame.asks, ladders_.asks()) || !load(frame.bids, ladders_.bids())) {
        invalidate();
        return BookResult::BadLevel;
    }
    last_end_ = frame.seq_end;
    return BookResult::Applied;
}

void UtaBook::invalidate() noexcept {
    ladders_.clear();
    last_end_ = 0;
    live_ = false;
}

std::optional<BookSnapshot> UtaBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!live_) {
        return std::nullopt;
    }
    return ladders_.snapshot(
        instrument_, static_cast<std::uint64_t>(last_end_), ts_event, ts_init);
}

std::optional<Bbo> UtaBook::bbo(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!live_ || ladders_.bids().empty() || ladders_.asks().empty()) {
        return std::nullopt;
    }
    const auto& bid = ladders_.bids().levels().front();
    const auto& ask = ladders_.asks().levels().front();
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bid.price;
    out.bid_qty = bid.qty;
    out.ask = ask.price;
    out.ask_qty = ask.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live) {
        return std::nullopt;
    }
    return ladders_.snapshot(
        instrument_, static_cast<std::uint64_t>(last_sequence_), ts_event, ts_init);
}

std::optional<Bbo> DepthBook::bbo(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live || ladders_.bids().empty() || ladders_.asks().empty()) {
        return std::nullopt;
    }
    const auto& bid = ladders_.bids().levels().front();
    const auto& ask = ladders_.asks().levels().front();
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bid.price;
    out.bid_qty = bid.qty;
    out.ask = ask.price;
    out.ask_qty = ask.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::kucoin
