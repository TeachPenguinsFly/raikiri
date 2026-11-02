#pragma once

#include <raikiri/domain/origin.hpp>
#include <raikiri/joe/parent.hpp>
#include <raikiri/joe/schedule.hpp>
#include <raikiri/joe/sor.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::joe {

struct Scale {
    Listing listing{};
    FixedE9 tick{};
    FixedE9 lot{};
};

class MarketView {
public:
    MarketView() = default;
    MarketView(const MarketView&) = delete;
    MarketView& operator=(const MarketView&) = delete;

    // venues that list `listing` and take orders, with the side an order on `side` would hit
    [[nodiscard]] virtual std::vector<VenueBook> books(const Listing& listing, Side side) const = 0;

    [[nodiscard]] virtual std::optional<Scale> scale(InstrumentId instrument) const = 0;

protected:
    ~MarketView() = default;
};

struct ChildOrder {
    InstrumentId instrument{};
    Side side{Side::Buy};
    std::int64_t lots{0};
    FixedE9 limit{};
    ChildTif tif{ChildTif::Ioc};
    std::uint16_t book{0};
    OrderOrigin origin{};
    // parent and child ordinal, so identical slices are not taken for a resend of one
    std::uint64_t repeat_key{0};
};

enum class SinkError : std::uint8_t { NotRunning, Risk, Refused };

class OrderSink {
public:
    OrderSink() = default;
    OrderSink(const OrderSink&) = delete;
    OrderSink& operator=(const OrderSink&) = delete;

    [[nodiscard]] virtual bool running() const noexcept = 0;

    // the child's client order id, raw
    [[nodiscard]] virtual std::expected<std::uint64_t, SinkError> submit(const ChildOrder& order) noexcept = 0;

    // the answer comes later as a close or a refusal; only that frees the child's slot
    virtual void cancel(std::uint64_t child_id) noexcept = 0;

    // a new price at the child's own size; false when it cannot go, and a cancel and a new child do instead
    [[nodiscard]] virtual bool modify(std::uint64_t child_id, InstrumentId instrument, FixedE9 limit) noexcept {
        (void)child_id;
        (void)instrument;
        (void)limit;
        return false;
    }

protected:
    ~OrderSink() = default;
};

// what a restart must not forget about a parent; its fills come back from the order events
struct ParentRecord {
    enum class Kind : std::uint8_t { Created, Started, ChildSent, StatusChanged };

    Kind kind{Kind::Created};
    std::uint64_t parent{0};
    UnixNanos ts{};
    ParentSpec spec{};
    FixedE9 qty{FixedE9::undef()};
    FixedE9 arrival{FixedE9::undef()};
    std::vector<UnixNanos> times{};
    std::vector<std::uint64_t> weights{};
    std::uint64_t random{0};
    Child child{};
    Status status{Status::Pending};
    std::string reason{};
};

class ParentLog {
public:
    ParentLog() = default;
    ParentLog(const ParentLog&) = delete;
    ParentLog& operator=(const ParentLog&) = delete;

    virtual void record(const ParentRecord& record) noexcept = 0;

protected:
    ~ParentLog() = default;
};

class VolumeProfile {
public:
    VolumeProfile() = default;
    VolumeProfile(const VolumeProfile&) = delete;
    VolumeProfile& operator=(const VolumeProfile&) = delete;

    [[nodiscard]] virtual std::vector<std::uint64_t>
    weights(const Listing& listing, UnixNanos start, std::uint64_t horizon_ns, std::uint32_t slices) const = 0;

protected:
    ~VolumeProfile() = default;
};

// curves per symbol, loaded from recorded trades; a symbol with none slices flat
class CurveProfile final : public VolumeProfile {
public:
    void add(const Listing& listing, UnixNanos ts, FixedE9 qty) { curves_[key(listing)].add(ts, qty); }

    [[nodiscard]] std::vector<std::uint64_t>
    weights(const Listing& listing, UnixNanos start, std::uint64_t horizon_ns, std::uint32_t slices) const override {
        const auto found = curves_.find(key(listing));
        return found == curves_.end() ? flat_weights(slices) : found->second.weights(start, horizon_ns, slices);
    }

private:
    [[nodiscard]] static std::string key(const Listing& listing) {
        return std::string{listing.symbol.view()} + (listing.market == MarketType::Spot ? ":spot" : ":perp");
    }

    std::map<std::string, VolumeCurve, std::less<>> curves_;
};

} // namespace raikiri::joe
