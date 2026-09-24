#pragma once

// instances keyed by strategy id; single-threaded, so the control server must be polled by the
// loop that owns them

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/lifecycle.hpp>
#include <raikiri/strategy/strategy.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace raikiri::strategy {

struct Spec {
    std::string name;   // this instance, in a person's words
    std::string type;   // a name the factory knows
    std::string config; // opaque; handed to the constructor verbatim
    std::uint16_t strategy_id{0};
};

enum class CreateError : std::uint8_t {
    UnknownType,
    IdTaken, // two instances under one id net into one position
    IdReserved, // 0 is the no-order sentinel; k_manual_strategy_id is a person's
    NoContext,  // the kernel would not open a window for this id
    Full,
    Refused, // the constructor read the config and said no; its words are in `detail`
};

struct CreateFailure {
    CreateError error{CreateError::UnknownType};
    std::string detail;
};

[[nodiscard]] std::string_view to_string(CreateError error) noexcept;

// no such strategy (404) is kept apart from a lifecycle refusal (409)
enum class ControlError : std::uint8_t {
    NoSuchStrategy,
    AlreadyRunning,
    NotRunning,
    Finished,
};

[[nodiscard]] constexpr ControlError as_control_error(LifecycleError error) noexcept {
    switch (error) {
    case LifecycleError::AlreadyRunning:
        return ControlError::AlreadyRunning;
    case LifecycleError::NotRunning:
        return ControlError::NotRunning;
    case LifecycleError::Finished:
        return ControlError::Finished;
    }
    return ControlError::Finished;
}

[[nodiscard]] std::string_view to_string(ControlError error) noexcept;

// no Strategy pointer, so nothing outside can call a hook
struct InstanceView {
    std::string name;
    std::string type;
    std::uint16_t strategy_id{0};
    Phase phase{Phase::Created};
    UnixNanos since{}; // when it entered `phase`
    std::size_t orders_cancelled_on_stop{0};
};

struct Kernel {
    // minted once per instance and kept alive by the kernel at least as long; null refuses the
    // create
    std::function<Context*(std::uint16_t strategy_id)> context_for;

    // cancels everything working under this strategy id and returns the count
    std::function<std::size_t(std::uint16_t strategy_id)> cancel_working;

    std::function<UnixNanos()> wall;
};

class Registry {
public:
    // fixed so dispatch's pointer survives a hook that creates a strategy
    static constexpr std::size_t k_max_instances = 64;

    explicit Registry(Kernel kernel);

    [[nodiscard]] std::expected<InstanceView, CreateFailure> create(const Spec& spec,
                                                                   const Factory& types);

    // the same, against the process factory
    [[nodiscard]] std::expected<InstanceView, CreateFailure> create(const Spec& spec);

    // holds the process's stop token so poll() tears this instance down through stop()
    [[nodiscard]] std::expected<Phase, ControlError> start(std::uint16_t strategy_id,
                                                           std::stop_token stop);

    // a stop does not flatten
    [[nodiscard]] std::expected<Phase, ControlError> stop(std::uint16_t strategy_id);

    // stops every running instance whose stop signal fired; returns how many
    std::size_t poll();

    // market data reaches every running instance
    void on_bbo(const Bbo& bbo);
    void on_trade(const TradeTick& trade);
    void on_funding(const FundingUpdate& funding);

    // an order event reaches only the strategy its client order id names; unowned ids are dropped
    void on_order_event(const OrderEvent& event);

    void on_timer(std::uint16_t strategy_id, std::uint32_t timer_id);

    [[nodiscard]] std::vector<InstanceView> list() const;
    [[nodiscard]] std::optional<InstanceView> find(std::uint16_t strategy_id) const;
    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }
    [[nodiscard]] std::size_t running() const noexcept;

private:
    struct Instance {
        Spec spec;
        std::unique_ptr<Strategy> strategy;
        Context* ctx{nullptr};
        Phase phase{Phase::Created};
        UnixNanos since{};
        // a stop unsubscribes before on_stop, so subscription and phase differ during teardown
        bool subscribed{false};
        // braced because it has no default and gcc makes the missing-field warning fatal
        std::stop_token stop{};
        std::size_t cancelled_on_stop{0};
    };

    [[nodiscard]] Instance* find_mut(std::uint16_t strategy_id) noexcept;
    [[nodiscard]] const Instance* find_mut(std::uint16_t strategy_id) const noexcept;
    [[nodiscard]] static InstanceView view_of(const Instance& instance);

    Kernel kernel_;
    std::vector<Instance> instances_;
};

} // namespace raikiri::strategy
