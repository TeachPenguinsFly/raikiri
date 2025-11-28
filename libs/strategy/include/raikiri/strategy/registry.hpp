#pragma once

// Every strategy instance this process holds, keyed by the strategy id that
// keys its book.
//
// THE KEY IS THE STRATEGY ID and not a name, because that is what the rest of
// the system already keys on: portfolio::PositionKey{strategy_id, instrument},
// the top 16 bits of every ClientOrderId, and every ledger row in the journal.
// A second identity here would be a second answer to "whose position is this".
//
// SINGLE-THREADED, like everything else on the kernel loop. The control
// surface in control.hpp calls straight into this from an http::Server
// handler, which is sound for exactly one reason: that server is polled by the
// loop that owns these strategies, so a handler runs between dispatches and
// never inside one. Installing that router on a server polled by any other
// thread breaks this type.

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

// What an operator asked for. Everything here is theirs except the id, which
// belongs to the system that has to account for it afterwards.
struct Spec {
    std::string name;   // this instance, in a person's words
    std::string type;   // a name the factory knows
    std::string config; // opaque; handed to the constructor verbatim
    std::uint16_t strategy_id{0};
};

enum class CreateError : std::uint8_t {
    UnknownType,
    IdTaken,    // two instances under one id net into ONE position — see PositionKey
    IdReserved, // 0 is the no-order sentinel; k_manual_strategy_id is a person's
    NoContext,  // the kernel would not open a window for this id
    Full,
    Refused, // the constructor read the config and said no; its words are in `detail`
};

// The error and the sentence, together. The enum is what code branches on; the
// text is what reaches the person who sent the request, and neither substitutes
// for the other.
struct CreateFailure {
    CreateError error{CreateError::UnknownType};
    std::string detail;
};

[[nodiscard]] std::string_view to_string(CreateError error) noexcept;

// Why a start or a stop did not happen.
//
// A lifecycle refusal and "no such strategy" are DIFFERENT FACTS and deserve
// different answers — 409 against 404 — so they must not share a code. The
// transition table in lifecycle.hpp cannot know the second one, which is why
// it is added here rather than there.
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

// One instance as everything outside this file sees it. No Strategy pointer:
// nothing out there may call a hook, because the ordering around one is the
// whole point of the lifecycle.
struct InstanceView {
    std::string name;
    std::string type;
    std::uint16_t strategy_id{0};
    Phase phase{Phase::Created};
    UnixNanos since{}; // when it entered `phase`
    // Working orders the stop pulled. Reported rather than logged: "stopped"
    // and "stopped, and here is what it was holding" are different answers,
    // and only the second one tells an operator whether to go looking.
    std::size_t orders_cancelled_on_stop{0};
};

// The three things a registry cannot build for itself, all supplied by the
// kernel that owns the order path.
struct Kernel {
    // The window this strategy will see, minted once per instance at create.
    // The KERNEL owns the object and must keep it alive at least as long as
    // the instance; returning null refuses the create, which is how a kernel
    // says "I have no book for that id".
    std::function<Context*(std::uint16_t strategy_id)> context_for;

    // Cancels everything working under this strategy id and returns how many.
    //
    // Supplied rather than reached through Context because only the kernel
    // holds the order store — and because a registry built without it must
    // fail to compile rather than stop a strategy quietly leaving its quotes
    // in the market.
    std::function<std::size_t(std::uint16_t strategy_id)> cancel_working;

    // Injected so a test can state the time rather than wait for it.
    std::function<UnixNanos()> wall;
};

class Registry {
public:
    // Capacity is fixed and the registry refuses to grow. Not about
    // allocation: dispatch holds a pointer into this vector while a hook runs,
    // and a hook that created a strategy through the control surface would
    // reallocate it underneath the loop that is dispatching to it.
    static constexpr std::size_t k_max_instances = 64;

    explicit Registry(Kernel kernel);

    [[nodiscard]] std::expected<InstanceView, CreateFailure> create(const Spec& spec,
                                                                   const Factory& types);

    // Same, against the process factory. The ordinary call; the overload above
    // exists so a test can register into a factory that is not global state.
    [[nodiscard]] std::expected<InstanceView, CreateFailure> create(const Spec& spec);

    // START TAKES THE STOP SIGNAL, and the token is the PROCESS's rather than
    // this instance's: SIGINT, a supervisor, a parent going away. It is held
    // so that `poll` can tear this instance down through the same ordered path
    // an explicit stop takes — without it, a strategy started over HTTP would
    // be one that shutting the process down never told to stop.
    [[nodiscard]] std::expected<Phase, ControlError> start(std::uint16_t strategy_id,
                                                           std::stop_token stop);

    // WHAT A STOP DOES NOT DO IS FLATTEN. Stated on the implementation, where
    // the order it happens in is visible.
    [[nodiscard]] std::expected<Phase, ControlError> stop(std::uint16_t strategy_id);

    // Stops every running instance whose stop signal has fired, and answers
    // with how many it stopped. The ONE place a process-wide shutdown reaches
    // a strategy, so the teardown is the same one a person gets.
    std::size_t poll();

    // --- dispatch -------------------------------------------------------
    //
    // MARKET DATA REACHES EVERY RUNNING INSTANCE: a price is public, and two
    // strategies watching one instrument is the ordinary case.
    void on_bbo(const Bbo& bbo);
    void on_trade(const TradeTick& trade);
    void on_funding(const FundingUpdate& funding);

    // AN ORDER EVENT REACHES ONE. The client order id packs the strategy that
    // placed the order, so the event names its own owner; handing it to the
    // others would show a strategy somebody else's fills. An id nobody here
    // owns — the manual desk's, or an order from a previous run that
    // reconciliation closed — is dropped, which is what "not mine" means.
    void on_order_event(const OrderEvent& event);

    void on_timer(std::uint16_t strategy_id, std::uint32_t timer_id);

    // --- reads ----------------------------------------------------------
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
        // SUBSCRIPTION IS SEPARATE FROM PHASE, and the two disagree for
        // exactly the length of a teardown: a stop unsubscribes before it runs
        // on_stop, so an event polled during that hook lands nowhere. One
        // field for both would deliver it.
        bool subscribed{false};
        // BRACED, unlike every other member here, because it is the only one
        // without a default of its own — and a designated initialiser that
        // does not name it is then a missing-field warning that gcc makes
        // fatal. A default stop_token is the right state for an instance that
        // has not been started: nothing can ask it to stop yet.
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
