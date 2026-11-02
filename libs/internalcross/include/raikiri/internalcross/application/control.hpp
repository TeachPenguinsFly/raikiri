#pragma once

// the switches people set: saved first, audited, and a change that lets more through needs saying so

#include <raikiri/audit/log.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/internalcross/application/desk.hpp>
#include <raikiri/internalcross/application/ports.hpp>

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::internalcross {

struct ChangeRequest {
    std::string by{};
    std::string reason{};
    bool confirm{false};
};

enum class ControlError : std::uint8_t { Invalid, NeedsConfirm, NotDurable };

struct ControlRefusal {
    ControlError error{ControlError::Invalid};
    std::string detail{};
    std::vector<std::string> raises{};
};

struct GlobalPatch {
    std::optional<bool> enabled{};
    std::optional<std::int64_t> max_spread_bp{};
    std::optional<std::uint64_t> max_quote_age_ms{};
    std::optional<std::uint64_t> hold_ms{};
    std::optional<bool> against_desk{};
};

class CrossControl {
public:
    CrossControl(CrossDesk& desk, SettingsStore& store, audit::Sink& audit, const Clock& clock);

    // the store's word, else `fallback` from the risk file; refuses what it cannot read rather than guess a switch
    [[nodiscard]] std::expected<void, std::string> load(const Settings& fallback);

    [[nodiscard]] std::expected<void, ControlRefusal> set(const GlobalPatch& patch, const ChangeRequest& request);
    [[nodiscard]] std::expected<void, ControlRefusal>
    set_book(const std::string& name, const BookPolicy& policy, const ChangeRequest& request);

    [[nodiscard]] const Settings& settings() const noexcept { return desk_->settings(); }

    [[nodiscard]] const CrossDesk& desk() const noexcept { return *desk_; }

private:
    [[nodiscard]] std::expected<void, ControlRefusal>
    commit(const Settings& next, std::string_view subject, const ChangeRequest& request);

    CrossDesk* desk_;
    SettingsStore* store_;
    audit::Sink* audit_;
    const Clock* clock_;
};

} // namespace raikiri::internalcross
