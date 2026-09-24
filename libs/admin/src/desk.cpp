#include <raikiri/admin/desk.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/shm/records.hpp>

#include <chrono>

namespace raikiri::admin {
namespace {

constexpr std::size_t k_keys_remembered = 1024;

} // namespace

std::string_view describe(DeskError error) noexcept {
    switch (error) {
    case DeskError::NoSuchInstrument:
        return "no attached gateway lists this instrument";
    case DeskError::NoSuchOrder:
        return "no such order on this desk";
    case DeskError::GatewayGone:
        return "the gateway that lists it is not attached";
    case DeskError::BadPrice:
        return "price is not a multiple of the instrument's tick";
    case DeskError::BadQty:
        return "quantity is not a multiple of the instrument's lot";
    case DeskError::PriceOnMarket:
        return "a market order must not carry a price";
    case DeskError::MissingPrice:
        return "a limit order needs a price";
    case DeskError::RingFull:
        return "the intent ring is full: strategyd is not draining";
    }
    return "refused";
}

ManualDesk::ManualDesk(const Registry& registry) noexcept
    : registry_{&registry},
      // seeded from epoch milliseconds so ids do not repeat across restarts
      seq_{static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count()) &
           0xFFFF'FFFF'FFFFULL} {}

ClientOrderId ManualDesk::mint() noexcept {
    return ClientOrderId::make(k_manual_strategy_id, ++seq_);
}

std::expected<ClientOrderId, DeskError>
ManualDesk::place(const DeskOrder& order, std::string_view key, UnixNanos now) {
    if (!key.empty()) {
        if (const auto found = seen_.find(std::string{key}); found != seen_.end()) {
            // same key: answer with the existing id and place nothing
            return ClientOrderId{found->second};
        }
    }

    const auto row = registry_->catalogue_row(order.instrument);
    if (!row.has_value() || row->instrument == nullptr) {
        return std::unexpected{DeskError::NoSuchInstrument};
    }
    if (row->gateway == nullptr || row->gateway->channels.order_intents == nullptr) {
        return std::unexpected{DeskError::GatewayGone};
    }
    const auto& base = base_of(*row->instrument);

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = order.side;
    cmd.order_type = order.type;
    cmd.tif = order.tif;
    cmd.flags = order.flags;
    cmd.instrument = order.instrument;
    cmd.ts_created = now;

    if (order.type == OrderType::Market) {
        if (!order.price.empty()) {
            return std::unexpected{DeskError::PriceOnMarket};
        }
    } else {
        if (order.price.empty()) {
            return std::unexpected{DeskError::MissingPrice};
        }
        const auto parsed = parse_decimal_e9(order.price);
        if (!parsed.has_value()) {
            return std::unexpected{DeskError::BadPrice};
        }
        // exact or refused; never round to the tick
        const auto ticks = base.price_to_ticks(*parsed);
        if (!ticks.has_value()) {
            return std::unexpected{DeskError::BadPrice};
        }
        cmd.price = *ticks;
    }

    const auto qty = parse_decimal_e9(order.qty);
    if (!qty.has_value()) {
        return std::unexpected{DeskError::BadQty};
    }
    const auto lots = base.qty_to_lots(*qty);
    if (!lots.has_value()) {
        return std::unexpected{DeskError::BadQty};
    }
    cmd.qty = *lots;

    cmd.client_order_id = mint();

    shm::CmdProducer producer{row->gateway->channels.order_intents};
    if (!producer.try_push(cmd).has_value()) {
        return std::unexpected{DeskError::RingFull};
    }

    if (!key.empty()) {
        seen_.emplace(std::string{key}, cmd.client_order_id.raw);
        order_.emplace_back(key);
        while (order_.size() > k_keys_remembered) {
            seen_.erase(order_.front());
            order_.pop_front();
        }
    }
    return cmd.client_order_id;
}

std::expected<void, DeskError> ManualDesk::cancel(ClientOrderId id, UnixNanos now) {
    if (id.strategy_id() != k_manual_strategy_id) {
        // not this desk's order
        return std::unexpected{DeskError::NoSuchOrder};
    }
    // the id does not carry the instrument, so every gateway gets the cancel and strategyd decides
    OrderCmd cmd{};
    cmd.kind = CmdKind::Cancel;
    cmd.client_order_id = id;
    cmd.ts_created = now;

    bool queued = false;
    for (std::size_t i = 0; i < registry_->size(); ++i) {
        const auto* gw = registry_->at(i);
        if (gw == nullptr || gw->channels.order_intents == nullptr) {
            continue;
        }
        shm::CmdProducer producer{gw->channels.order_intents};
        queued = producer.try_push(cmd).has_value() || queued;
    }
    if (!queued) {
        return std::unexpected{DeskError::RingFull};
    }
    return {};
}

} // namespace raikiri::admin
