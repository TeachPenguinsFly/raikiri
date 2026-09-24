#include <raikiri/strategy/registry.hpp>

#include <algorithm>
#include <utility>

namespace raikiri::strategy {

std::string_view to_string(CreateError error) noexcept {
    switch (error) {
    case CreateError::UnknownType:
        return "no strategy type by that name is registered";
    case CreateError::IdTaken:
        return "a strategy already holds that id";
    case CreateError::IdReserved:
        return "that id is the manual desk's and is never handed to a strategy";
    case CreateError::NoContext:
        return "the kernel opened no window for that id";
    case CreateError::Full:
        return "this process holds as many strategies as it can";
    case CreateError::Refused:
        return "the strategy refused its configuration";
    }
    return "?";
}

std::string_view to_string(ControlError error) noexcept {
    switch (error) {
    case ControlError::NoSuchStrategy:
        return "no strategy holds that id";
    case ControlError::AlreadyRunning:
        return "already running";
    case ControlError::NotRunning:
        return "not running";
    case ControlError::Finished:
        return "already stopped; a stopped strategy is neither started nor stopped again";
    }
    return "?";
}

Registry::Registry(Kernel kernel) : kernel_{std::move(kernel)} {
    // Reserved once, never grown: see k_max_instances.
    instances_.reserve(k_max_instances);
}

Registry::Instance* Registry::find_mut(std::uint16_t strategy_id) noexcept {
    const auto found = std::ranges::find_if(
        instances_, [&](const Instance& one) { return one.spec.strategy_id == strategy_id; });
    return found == instances_.end() ? nullptr : &*found;
}

const Registry::Instance* Registry::find_mut(std::uint16_t strategy_id) const noexcept {
    const auto found = std::ranges::find_if(
        instances_, [&](const Instance& one) { return one.spec.strategy_id == strategy_id; });
    return found == instances_.end() ? nullptr : &*found;
}

InstanceView Registry::view_of(const Instance& instance) {
    return {.name = instance.spec.name,
            .type = instance.spec.type,
            .strategy_id = instance.spec.strategy_id,
            .phase = instance.phase,
            .since = instance.since,
            .orders_cancelled_on_stop = instance.cancelled_on_stop};
}

std::expected<InstanceView, CreateFailure> Registry::create(const Spec& spec) {
    return create(spec, factory());
}

std::expected<InstanceView, CreateFailure> Registry::create(const Spec& spec,
                                                            const Factory& types) {
    if (spec.strategy_id == 0) {
        // zero is the no-order sentinel
        return std::unexpected{
            CreateFailure{.error = CreateError::IdReserved,
                          .detail = "0 is the no-order sentinel, never a strategy"}};
    }
    if (spec.strategy_id == k_manual_strategy_id) {
        // a strategy sharing the desk's id would net into the manual book
        return std::unexpected{CreateFailure{
            .error = CreateError::IdReserved,
            .detail = std::string{to_string(CreateError::IdReserved)}}};
    }
    if (find_mut(spec.strategy_id) != nullptr) {
        return std::unexpected{
            CreateFailure{.error = CreateError::IdTaken,
                          .detail = std::string{to_string(CreateError::IdTaken)}}};
    }
    if (instances_.size() >= k_max_instances) {
        return std::unexpected{CreateFailure{.error = CreateError::Full,
                                             .detail = std::string{to_string(CreateError::Full)}}};
    }
    if (!types.contains(spec.type)) {
        return std::unexpected{CreateFailure{.error = CreateError::UnknownType,
                                             .detail = "no strategy type named " + spec.type}};
    }

    // the context before the strategy, so a failed context needs no unwind
    Context* ctx = kernel_.context_for ? kernel_.context_for(spec.strategy_id) : nullptr;
    if (ctx == nullptr) {
        return std::unexpected{
            CreateFailure{.error = CreateError::NoContext,
                          .detail = std::string{to_string(CreateError::NoContext)}}};
    }

    auto built = types.make(spec.type, {.config = spec.config, .strategy_id = spec.strategy_id});
    if (!built.has_value()) {
        return std::unexpected{
            CreateFailure{.error = CreateError::Refused, .detail = built.error()}};
    }

    instances_.push_back(Instance{.spec = spec,
                                  .strategy = std::move(*built),
                                  .ctx = ctx,
                                  .phase = Phase::Created,
                                  .since = kernel_.wall ? kernel_.wall() : UnixNanos{}});
    return view_of(instances_.back());
}

std::expected<Phase, ControlError> Registry::start(std::uint16_t strategy_id,
                                                   std::stop_token stop) {
    auto* instance = find_mut(strategy_id);
    if (instance == nullptr) {
        return std::unexpected{ControlError::NoSuchStrategy};
    }
    const auto next = step(instance->phase, Transition::Start);
    if (!next.has_value()) {
        return std::unexpected{as_control_error(next.error())};
    }
    // subscribe last so no event reaches a strategy that is still starting
    instance->phase = *next;
    instance->since = kernel_.wall ? kernel_.wall() : UnixNanos{};
    instance->stop = std::move(stop);
    instance->strategy->on_start(*instance->ctx);
    instance->subscribed = true;
    return *next;
}

std::expected<Phase, ControlError> Registry::stop(std::uint16_t strategy_id) {
    auto* instance = find_mut(strategy_id);
    if (instance == nullptr) {
        return std::unexpected{ControlError::NoSuchStrategy};
    }
    const auto next = step(instance->phase, Transition::Stop);
    if (!next.has_value()) {
        return std::unexpected{as_control_error(next.error())};
    }

    // unsubscribe first so a teardown never acts on new data
    instance->subscribed = false;
    instance->strategy->on_stop(*instance->ctx);
    // cancel after on_stop so orders it placed are pulled too; a stop does not flatten
    instance->cancelled_on_stop = kernel_.cancel_working ? kernel_.cancel_working(strategy_id) : 0;
    instance->phase = *next;
    instance->since = kernel_.wall ? kernel_.wall() : UnixNanos{};
    return *next;
}

std::size_t Registry::poll() {
    std::size_t stopped = 0;
    for (auto& instance : instances_) {
        if (instance.phase != Phase::Running || !instance.stop.stop_requested()) {
            continue;
        }
        // through stop() so there is one teardown; the reserved vector does not move
        if (stop(instance.spec.strategy_id).has_value()) {
            ++stopped;
        }
    }
    return stopped;
}

void Registry::on_bbo(const Bbo& bbo) {
    for (auto& instance : instances_) {
        if (instance.subscribed) {
            instance.strategy->on_bbo(*instance.ctx, bbo);
        }
    }
}

void Registry::on_trade(const TradeTick& trade) {
    for (auto& instance : instances_) {
        if (instance.subscribed) {
            instance.strategy->on_trade(*instance.ctx, trade);
        }
    }
}

void Registry::on_funding(const FundingUpdate& funding) {
    for (auto& instance : instances_) {
        if (instance.subscribed) {
            instance.strategy->on_funding(*instance.ctx, funding);
        }
    }
}

void Registry::on_order_event(const OrderEvent& event) {
    auto* instance = find_mut(event.client_order_id.strategy_id());
    if (instance != nullptr && instance->subscribed) {
        instance->strategy->on_order_event(*instance->ctx, event);
    }
}

void Registry::on_timer(std::uint16_t strategy_id, std::uint32_t timer_id) {
    auto* instance = find_mut(strategy_id);
    if (instance != nullptr && instance->subscribed) {
        instance->strategy->on_timer(*instance->ctx, timer_id);
    }
}

std::vector<InstanceView> Registry::list() const {
    std::vector<InstanceView> out;
    out.reserve(instances_.size());
    for (const auto& instance : instances_) {
        out.push_back(view_of(instance));
    }
    return out; // creation order, stable
}

std::optional<InstanceView> Registry::find(std::uint16_t strategy_id) const {
    const auto* instance = find_mut(strategy_id);
    return instance == nullptr ? std::nullopt : std::optional{view_of(*instance)};
}

std::size_t Registry::running() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        instances_, [](const Instance& one) { return one.phase == Phase::Running; }));
}

} // namespace raikiri::strategy
