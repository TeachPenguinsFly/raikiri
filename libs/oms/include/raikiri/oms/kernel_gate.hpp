#pragma once

// Whether the kernel still trusts its own picture of what is held and working,
// and therefore whether it may add risk.
//
// NOT the order FSM. That one moves a single order through Submitted →
// Accepted → Filled and answers "is this transition legal". This one is about
// the PROCESS, per gateway, and answers a different question: the book we are
// trading off may have gone wrong, so may we send anything new at all.
//
// The case it exists for is already named in strategyd and not implemented:
//
//     ++exec_overruns; // policy: halt+reconcile; demo just counts
//
// An overrun on the execution ring is a LOST EVENT, and a lost event may be a
// fill. The local book then holds a position the venue does not, or misses one
// it does, and every later decision is priced off that. Counting it is not a
// policy; it is a record that we noticed and carried on.
//
// WHAT NEVER STOPS IS CANCEL. Every state here permits it, which is why
// `may_cancel` is a field rather than an implied truth: a kernel that has lost
// confidence must still be able to reduce what is already outstanding, and the
// gateway already drains the cancel ring before the create ring for the same
// reason.
//
// One machine per gateway. A break on bybit-perp stops new orders there and
// says nothing about coinbase-spot. A strategy spanning both legs is expected
// to refuse to OPEN while either leg is stopped — opening the second leg with
// the first unavailable is how a position ends up one-legged — but that is the
// strategy's rule to state, not this type's to assume.

#include <raikiri/common/time.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::oms {

enum class KernelState : std::uint8_t {
    // The book is trusted. Normal trading.
    Running,

    // Trust is suspended and MAY RETURN ON ITS OWN: a reconciliation that
    // agrees puts it back. This is what an overrun earns — something was
    // missed, and asking the venue settles whether it mattered.
    Degraded,

    // Trust is gone and does NOT return on its own. Reaching Running again
    // takes two things in order: a reconciliation that agrees, and then a
    // person. A halt a lucky poll could clear is not a halt.
    Halted,
};

enum class KernelEvent : std::uint8_t {
    // A record was dropped between the gateway and here. Might have been a
    // fill; the only way to know is to ask the venue.
    ExecOverrun,

    // The segment owner stopped answering. Everything read from it belongs to
    // a generation that no longer exists.
    GatewayLost,

    // The venue's position agrees with the book.
    ReconAgreed,

    // A break CONFIRMED — seen at a different venue boundary from the one that
    // first disagreed (oms::ReconGate). One boundary read twice confirms
    // nothing, so a single mismatch must not arrive here.
    ReconFailed,

    // A person says resume. It is a permission, never a repair: on its own it
    // moves nothing.
    OperatorResume,
};

// What a state permits. Two fields rather than one, because the interesting
// property of every stopped state is the half that stays open.
struct KernelVerdict {
    bool may_open{false};   // Create and Amend — anything that ADDS exposure
    bool may_cancel{true};  // never false in any state; see the header
};

// The machine's whole state: where it is, and whether a reconciliation has
// agreed since it stopped. The second half is what makes `OperatorResume` a
// permission rather than an override — a person may not sign off on a book
// nobody has checked.
struct KernelStep {
    KernelState state{KernelState::Running};
    bool recon_agreed{true};
};

// The transition, pure and total: 3 states x 2 flags x 5 events, no I/O, no
// clock, no allocation. Exhaustively testable, which is the point — the
// alternative is a handful of bools whose illegal combinations are discovered
// in production.
[[nodiscard]] constexpr KernelStep step(KernelStep from, KernelEvent event) noexcept {
    using enum KernelState;
    using enum KernelEvent;
    switch (event) {
    case GatewayLost:
        // Straight to Halted from anywhere. A gateway that went away took its
        // generation with it, and nothing read before it left can be trusted
        // to describe what is there now.
        return {.state = Halted, .recon_agreed = false};
    case ExecOverrun:
        // Never a promotion. From Halted it changes nothing: the worse state
        // already forbids everything this one would.
        return {.state = from.state == Halted ? Halted : Degraded, .recon_agreed = false};
    case ReconFailed:
        return {.state = Halted, .recon_agreed = false};
    case ReconAgreed:
        // Degraded clears itself here. Halted does NOT — agreement is recorded
        // so a person can act on it, and that is all.
        return from.state == Degraded ? KernelStep{.state = Running, .recon_agreed = true}
                                      : KernelStep{.state = from.state, .recon_agreed = true};
    case OperatorResume:
        // The only event a person raises, and it is refused unless the book has
        // been checked since the halt. Resuming an unverified book is the
        // original defect with a signature on it.
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

// One gateway's machine, plus the two things a person and a screen need that a
// pure transition cannot carry: what moved it here, and when.
class KernelGate {
public:
    [[nodiscard]] KernelState state() const noexcept { return now_.state; }

    [[nodiscard]] KernelVerdict verdict() const noexcept { return verdict_of(now_.state); }

    [[nodiscard]] bool may_open() const noexcept { return verdict().may_open; }

    // Applies the event. True when the STATE moved — the caller logs and
    // journals on that, not on every event, or a per-tick overrun writes a line
    // per tick.
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

    // Why the machine is where it is, and since when. Both are the reason this
    // is a state machine rather than a bool: a screen that says "halted" and
    // cannot say what halted it sends someone reading logs.
    [[nodiscard]] KernelEvent because() const noexcept { return because_; }

    [[nodiscard]] UnixNanos since() const noexcept { return since_; }

    // True once the venue has agreed since the halt — i.e. an operator resume
    // would now be honoured rather than ignored.
    [[nodiscard]] bool resumable() const noexcept {
        return now_.state == KernelState::Halted && now_.recon_agreed;
    }

private:
    KernelStep now_{};
    UnixNanos since_{};
    KernelEvent because_{KernelEvent::ReconAgreed};
};

} // namespace raikiri::oms
