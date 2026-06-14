#pragma once

// A blocking wait on the cold path gives up as soon as any scope open on
// its thread says to, so a stop reaches a request already in flight rather
// than waiting out its timeout.

#include <functional>

namespace raikiri {

class AbandonScope {
public:
    explicit AbandonScope(const std::function<bool()>& abandon) noexcept
        : abandon_{&abandon}, outer_{innermost()} {
        innermost() = this;
    }
    ~AbandonScope() { innermost() = outer_; }
    AbandonScope(const AbandonScope&) = delete;
    AbandonScope& operator=(const AbandonScope&) = delete;

    [[nodiscard]] static bool abandoned() {
        for (const auto* scope = innermost(); scope != nullptr; scope = scope->outer_) {
            if ((*scope->abandon_)()) {
                return true;
            }
        }
        return false;
    }

private:
    static AbandonScope*& innermost() noexcept {
        thread_local AbandonScope* scope = nullptr;
        return scope;
    }

    const std::function<bool()>* abandon_;
    AbandonScope* outer_;
};

} // namespace raikiri
