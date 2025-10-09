#pragma once

// Where one strategy instance is in its life, and which moves are legal.
//
// Pure and total: three phases, two transitions, no I/O, no clock, no
// allocation — the same shape as oms::kernel_gate and for the same reason. A
// lifecycle whose illegal transitions are discovered in production is exactly
// what this replaces.
//
// STOPPED IS TERMINAL, and that is a decision rather than an omission. A
// stopped instance has had on_stop run and its working orders pulled; whatever
// it remembers describes a market it stopped watching, and starting it again
// would resume a picture that has since moved. Running the same type again
// costs one create, and that one starts from a state that matches the world.
//
// A STOP IS THE UNWIND OF A START, which is why stopping something that never
// started is refused rather than waved through: on_stop on a strategy whose
// on_start never ran hands it a teardown of arrangements it never made.

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::strategy {

enum class Phase : std::uint8_t {
    Created, // constructed; subscribed to nothing, holds no orders
    Running, // on_start has returned and events reach it
    Stopped, // terminal — see the header
};

enum class Transition : std::uint8_t { Start, Stop };

enum class LifecycleError : std::uint8_t {
    AlreadyRunning, // start, on something already running
    NotRunning,     // stop, on something that never started
    Finished,       // any move at all, on something stopped
};

[[nodiscard]] constexpr std::expected<Phase, LifecycleError> step(Phase from,
                                                                  Transition move) noexcept {
    using Result = std::expected<Phase, LifecycleError>;
    if (from == Phase::Stopped) {
        // Before the switch on purpose: from here BOTH moves are refused, and
        // the reason is the same one, so neither arm below should have to
        // remember it.
        return Result{std::unexpect, LifecycleError::Finished};
    }
    switch (move) {
    case Transition::Start:
        return from == Phase::Created ? Result{Phase::Running}
                                      : Result{std::unexpect, LifecycleError::AlreadyRunning};
    case Transition::Stop:
        return from == Phase::Running ? Result{Phase::Stopped}
                                      : Result{std::unexpect, LifecycleError::NotRunning};
    }
    return Result{std::unexpect, LifecycleError::Finished};
}

[[nodiscard]] constexpr std::string_view to_string(Phase phase) noexcept {
    switch (phase) {
    case Phase::Created:
        return "created";
    case Phase::Running:
        return "running";
    case Phase::Stopped:
        return "stopped";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(LifecycleError error) noexcept {
    switch (error) {
    case LifecycleError::AlreadyRunning:
        return "already running";
    case LifecycleError::NotRunning:
        return "not running";
    case LifecycleError::Finished:
        return "already stopped; a stopped strategy is neither started nor stopped again";
    }
    return "?";
}

} // namespace raikiri::strategy
