#include <raikiri/options/chain.hpp>

#include <algorithm>
#include <utility>

namespace raikiri::options {

namespace {

struct Placed {
    OptionKey key;
    InstrumentId id;
};

} // namespace

OptionChain OptionChain::build(std::span<const Instrument> catalogue) {
    std::vector<Placed> placed;
    for (const auto& instrument : catalogue) {
        const auto* option = std::get_if<OptionInstrument>(&instrument);
        if (option == nullptr) {
            continue;
        }
        if (const auto key = key_of(*option); key.has_value()) {
            placed.push_back({*key, option->base.id});
        }
    }
    std::ranges::stable_sort(placed, {}, &Placed::key);

    OptionChain chain;
    const OptionKey* previous = nullptr;
    for (const auto& one : placed) {
        if (previous != nullptr && *previous == one.key) {
            continue;
        }
        previous = &one.key;
        if (chain.underlyings_.empty() || chain.underlyings_.back().underlying != one.key.underlying) {
            chain.underlyings_.push_back({.underlying = one.key.underlying});
        }
        auto& expiries = chain.underlyings_.back().expiries;
        if (expiries.empty() || expiries.back().expiry != one.key.expiry) {
            expiries.push_back({.expiry = one.key.expiry});
        }
        auto& rows = expiries.back().rows;
        if (rows.empty() || rows.back().strike != one.key.strike) {
            rows.push_back({.strike = one.key.strike});
        }
        (one.key.put_call == PutCall::Call ? rows.back().call : rows.back().put) = one.id;
        ++chain.size_;
    }
    return chain;
}

const ChainUnderlying* OptionChain::underlying(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(underlyings_, [&](const auto& u) { return u.underlying.view() == name; });
    return found == underlyings_.end() ? nullptr : &*found;
}

const ChainExpiry* OptionChain::expiry(std::string_view underlying_name, UnixNanos expiry) const noexcept {
    const auto* found = underlying(underlying_name);
    if (found == nullptr) {
        return nullptr;
    }
    const auto at = std::ranges::lower_bound(found->expiries, expiry, {}, &ChainExpiry::expiry);
    return at == found->expiries.end() || at->expiry != expiry ? nullptr : &*at;
}

std::optional<InstrumentId> OptionChain::find(const OptionKey& key) const noexcept {
    const auto* slice = expiry(key.underlying.view(), key.expiry);
    if (slice == nullptr) {
        return std::nullopt;
    }
    const auto row = std::ranges::lower_bound(slice->rows, key.strike, {}, &ChainRow::strike);
    if (row == slice->rows.end() || row->strike != key.strike) {
        return std::nullopt;
    }
    return key.put_call == PutCall::Call ? row->call : row->put;
}

std::vector<MatchedOption> match(const OptionChain& left, const OptionChain& right) {
    std::vector<MatchedOption> out;
    for (const auto& underlying : left.underlyings()) {
        for (const auto& slice : underlying.expiries) {
            for (const auto& row : slice.rows) {
                for (const auto side : {PutCall::Call, PutCall::Put}) {
                    const auto& mine = side == PutCall::Call ? row.call : row.put;
                    if (!mine.has_value()) {
                        continue;
                    }
                    const OptionKey key{.underlying = underlying.underlying,
                                        .expiry = slice.expiry,
                                        .strike = row.strike,
                                        .put_call = side};
                    if (const auto theirs = right.find(key); theirs.has_value()) {
                        out.push_back({.key = key, .left = *mine, .right = *theirs});
                    }
                }
            }
        }
    }
    return out;
}

} // namespace raikiri::options
