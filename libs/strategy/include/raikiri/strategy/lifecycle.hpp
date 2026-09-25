#pragma once

// three phases, two transitions; stopped is terminal and stopping a never-started instance is
// refused

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::strategy {

enum class Phase : std::uint8_t {
    Created, // constructed; subscribed to nothing, holds no orders
    Running, // on_start has returned and events reach it
    Stopped, // terminal
};

enum class Transition : std::uint8_t { Start, Stop };

enum class LifecycleError : std::uint8_t {
    AlreadyRunning, // start, on something already running
    NotRunning,     // stop, on something that never started
    Finished,       // any move at all, on something stopped
};

[[nodiscard]] constexpr std::expected<Phase, LifecycleError> step(Phase from, Transition move) noexcept {
    using Result = std::expected<Phase, LifecycleError>;
    if (from == Phase::Stopped) {
        return Result{std::unexpect, LifecycleError::Finished};
    }
    switch (move) {
    case Transition::Start:
        return from == Phase::Created ? Result{Phase::Running} : Result{std::unexpect, LifecycleError::AlreadyRunning};
    case Transition::Stop:
        return from == Phase::Running ? Result{Phase::Stopped} : Result{std::unexpect, LifecycleError::NotRunning};
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
