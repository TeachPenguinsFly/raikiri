#include <raikiri/admin/trade_tape.hpp>

#include <algorithm>

namespace raikiri::admin {

void TradeRing::push(const TradePrint& print) noexcept {
    prints_[head_ % k_trade_rows] = print;
    ++head_;
}

std::vector<TradePrint> TradeRing::since(std::uint64_t cursor) const {
    const auto held = std::min<std::uint64_t>(head_, k_trade_rows);
    const auto from = std::clamp(cursor, head_ - held, head_);
    std::vector<TradePrint> out;
    out.reserve(head_ - from);
    for (auto seq = from; seq < head_; ++seq) {
        out.push_back(prints_[seq % k_trade_rows]);
    }
    return out;
}

std::shared_ptr<TradeRing> TradeTape::watch(InstrumentId id) {
    auto& ring = rings_[id.packed];
    if (ring == nullptr) {
        ring = std::make_shared<TradeRing>();
    }
    return ring;
}

bool TradeTape::push(InstrumentId id, const TradePrint& print) noexcept {
    const auto found = rings_.find(id.packed);
    if (found == rings_.end()) {
        return false;
    }
    found->second->push(print);
    return true;
}

std::size_t TradeTape::prune() {
    return std::erase_if(rings_, [](const auto& entry) { return entry.second.use_count() == 1; });
}

std::vector<InstrumentId> TradeTape::instruments() const {
    std::vector<InstrumentId> out;
    out.reserve(rings_.size());
    for (const auto& [packed, ring] : rings_) {
        out.push_back(InstrumentId{packed});
    }
    return out;
}

} // namespace raikiri::admin
