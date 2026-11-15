#include <raikiri/candles/candle.hpp>

#include <algorithm>

namespace raikiri::candles {

void MinuteBuilder::trade(InstrumentId instrument, std::int64_t price, std::int64_t qty, UnixNanos ts) {
    if (price <= 0) {
        return;
    }
    ++prints_;
    const auto minute = minute_of(ts);
    auto [it, fresh] = open_.try_emplace(instrument.packed);
    auto& c = it->second;
    if (!fresh && minute < c.minute) {
        ++late_;
        return;
    }
    if (!fresh && minute > c.minute) {
        closed_.push_back(c);
        fresh = true;
    }
    if (fresh) {
        c = Candle{.instrument = instrument,
                   .minute = minute,
                   .open = price,
                   .high = price,
                   .low = price,
                   .close = price,
                   .volume = 0,
                   .trades = 0,
                   .source = Source::Live};
    }
    c.high = std::max(c.high, price);
    c.low = std::min(c.low, price);
    c.close = price;
    c.volume += std::max<std::int64_t>(qty, 0);
    ++c.trades;
}

void MinuteBuilder::close_through(UnixNanos now, std::uint64_t grace_ns) {
    const auto ended = minute_of(UnixNanos{now.ns > grace_ns ? now.ns - grace_ns : 0});
    for (auto it = open_.begin(); it != open_.end();) {
        if (it->second.minute < ended) {
            closed_.push_back(it->second);
            it = open_.erase(it);
        } else {
            ++it;
        }
    }
}

void MinuteBuilder::close_all() {
    for (const auto& [_, c] : open_) {
        closed_.push_back(c);
    }
    open_.clear();
}

std::vector<Candle> MinuteBuilder::take() {
    std::vector<Candle> out;
    out.swap(closed_);
    return out;
}

} // namespace raikiri::candles
