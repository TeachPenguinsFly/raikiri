#pragma once

// whether the kernel may add risk on one gateway; cancel is permitted in every state

#include <raikiri/common/time.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::oms {

enum class KernelState : std::uint8_t {
    Running,

    // may return on its own when a reconciliation agrees
    Degraded,

    // returns only after a reconciliation agrees and then an operator resumes
    Halted,
};

enum class KernelEvent : std::uint8_t {
    // a record was dropped between the gateway and here
    ExecOverrun,

    // the segment owner stopped answering
    GatewayLost,

    // The venue's position agrees with the book.
    ReconAgreed,

    // a break confirmed at a second venue boundary (oms::ReconGate)
    ReconFailed,

    // a permission; it moves nothing on its own
    OperatorResume,
};

struct KernelVerdict {
    bool may_open{false}; // create and amend, anything that adds exposure
    bool may_cancel{true}; // true in every state
};

struct KernelStep {
    KernelState state{KernelState::Running};
    bool recon_agreed{true};
};

[[nodiscard]] constexpr KernelStep step(KernelStep from, KernelEvent event) noexcept {
    using enum KernelState;
    using enum KernelEvent;
    switch (event) {
    case GatewayLost:
        // a lost gateway took its generation with it
        return {.state = Halted, .recon_agreed = false};
    case ExecOverrun:
        // never a promotion
        return {.state = from.state == Halted ? Halted : Degraded, .recon_agreed = false};
    case ReconFailed:
        return {.state = Halted, .recon_agreed = false};
    case ReconAgreed:
        // degraded clears itself; halted only records the agreement
        return from.state == Degraded ? KernelStep{.state = Running, .recon_agreed = true}
                                      : KernelStep{.state = from.state, .recon_agreed = true};
    case OperatorResume:
        // refused unless a reconciliation agreed since the halt
        return from.state == Halted && from.recon_agreed
                   ? KernelStep{.state = Running, .recon_agreed = true}
                   : from;
    }
    return from;
}

[[nodiscard]] constexpr KernelVerdict verdict_of(KernelState state) noexcept {
    return {.may_open = state == KernelState::Running, .may_cancel = true};
}

[[nodiscard]] constexpr std::string_view to_string(KernelState state) noexcept {
    using enum KernelState;
    switch (state) {
    case Running:
        return "running";
    case Degraded:
        return "degraded";
    case Halted:
        return "halted";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(KernelEvent event) noexcept {
    using enum KernelEvent;
    switch (event) {
    case ExecOverrun:
        return "execution event dropped";
    case GatewayLost:
        return "gateway stopped answering";
    case ReconAgreed:
        return "venue agreed with the book";
    case ReconFailed:
        return "venue disagreed, confirmed";
    case OperatorResume:
        return "operator resumed";
    }
    return "?";
}

class KernelGate {
public:
    [[nodiscard]] KernelState state() const noexcept { return now_.state; }

    [[nodiscard]] KernelVerdict verdict() const noexcept { return verdict_of(now_.state); }

    [[nodiscard]] bool may_open() const noexcept { return verdict().may_open; }

    // true when the state moved, which is when the caller logs
    bool on(KernelEvent event, UnixNanos at) noexcept {
        const auto next = step(now_, event);
        const bool moved = next.state != now_.state;
        if (moved) {
            since_ = at;
            because_ = event;
        }
        now_ = next;
        return moved;
    }

    [[nodiscard]] KernelEvent because() const noexcept { return because_; }

    [[nodiscard]] UnixNanos since() const noexcept { return since_; }

    // true once the venue has agreed since the halt
    [[nodiscard]] bool resumable() const noexcept {
        return now_.state == KernelState::Halted && now_.recon_agreed;
    }

private:
    KernelStep now_{};
    UnixNanos since_{};
    KernelEvent because_{KernelEvent::ReconAgreed};
};

} // namespace raikiri::oms
