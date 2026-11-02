#pragma once

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/domain/origin.hpp>
#include <raikiri/internalcross/domain/match.hpp>
#include <raikiri/internalcross/domain/settings.hpp>
#include <raikiri/oms/cross_record.hpp>
#include <raikiri/portfolio/position.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

namespace raikiri::internalcross {

struct LegFacts {
    QtyLots position{QtyLots::from_raw(0)};
    QtyLots working_buy{QtyLots::from_raw(0)};
    QtyLots working_sell{QtyLots::from_raw(0)};
};

// the running kernel, as crossing sees it; every call is on the strategy loop
class CrossKernel {
public:
    CrossKernel() = default;
    CrossKernel(const CrossKernel&) = delete;
    CrossKernel& operator=(const CrossKernel&) = delete;

    // this listing and its siblings on other venues that take orders; the listing itself comes back marked own
    [[nodiscard]] virtual std::size_t touches(InstrumentId instrument, std::span<Touch> out) = 0;

    // the book's position and working lots, without the order `leaving_out`
    [[nodiscard]] virtual LegFacts
    facts(std::uint16_t book, InstrumentId instrument, ClientOrderId leaving_out) const = 0;

    // the counterparty's own risk gate, on the lots this leg would take; `standing` is its resting order, if it has one
    [[nodiscard]] virtual bool leg_allowed(const OrderCmd& leg, ClientOrderId standing) = 0;

    [[nodiscard]] virtual BookKind kind_of(std::uint16_t book) const = 0;
    [[nodiscard]] virtual std::string name_of(std::uint16_t book) const = 0;
    [[nodiscard]] virtual std::optional<std::uint16_t> hedge_book() const = 0;

    // after both legs are in the books and the journal; `placed` is false for the hedge leg, an order nobody placed
    virtual void booked(const OrderEvent& leg, bool placed) = 0;

protected:
    ~CrossKernel() = default;
};

struct BookedLeg {
    OrderEvent event{};
    const portfolio::Position* position{nullptr};
    portfolio::Position::Applied applied{};
};

class CrossLog {
public:
    CrossLog() = default;
    CrossLog(const CrossLog&) = delete;
    CrossLog& operator=(const CrossLog&) = delete;

    // the hedge leg's order, before its cross
    virtual void order(const OrderCmd& cmd, const OrderOrigin& origin) noexcept = 0;

    virtual void crossed(const oms::CrossRecord& record,
                         const BookedLeg& buy,
                         const BookedLeg& sell,
                         const Instrument& listed) noexcept = 0;

protected:
    ~CrossLog() = default;
};

class SettingsStore {
public:
    SettingsStore() = default;
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // nullopt when nobody has decided anything yet
    [[nodiscard]] virtual std::expected<std::optional<Settings>, std::string> load() = 0;
    [[nodiscard]] virtual std::expected<void, std::string> save(const Settings& settings) = 0;

protected:
    ~SettingsStore() = default;
};

} // namespace raikiri::internalcross
