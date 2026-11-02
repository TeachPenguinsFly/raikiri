#include <raikiri/internalcross/application/control.hpp>

#include <fmt/format.h>

namespace raikiri::internalcross {

CrossControl::CrossControl(CrossDesk& desk, SettingsStore& store, audit::Sink& audit, const Clock& clock)
    : desk_{&desk}, store_{&store}, audit_{&audit}, clock_{&clock} {}

std::expected<void, std::string> CrossControl::load(const Settings& fallback) {
    auto saved = store_->load();
    if (!saved.has_value()) {
        return std::unexpected{saved.error()};
    }
    const auto& chosen = saved->has_value() ? **saved : fallback;
    if (auto why = validate(chosen); !why.empty()) {
        return std::unexpected{"internal crossing: " + why};
    }
    desk_->configure(chosen);
    return {};
}

std::expected<void, ControlRefusal>
CrossControl::commit(const Settings& next, std::string_view subject, const ChangeRequest& request) {
    if (request.by.empty()) {
        return std::unexpected{ControlRefusal{.detail = "a change needs a name"}};
    }
    if (auto why = validate(next); !why.empty()) {
        return std::unexpected{ControlRefusal{.detail = std::move(why)}};
    }
    const auto& before = desk_->settings();
    if (auto more = raises(before, next); !more.empty() && !request.confirm) {
        return std::unexpected{ControlRefusal{
            .error = ControlError::NeedsConfirm, .detail = "this lets more cross", .raises = std::move(more)}};
    }
    if (const auto saved = store_->save(next); !saved.has_value()) {
        return std::unexpected{ControlRefusal{.error = ControlError::NotDurable, .detail = saved.error()}};
    }
    auto clean = request.reason;
    for (auto& c : clean) {
        c = c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20 ? '?' : c;
    }
    const auto detail = fmt::format(
        R"({{"enabled":{},"against_desk":{},"max_spread_bp":{},"max_quote_age_ms":{},"hold_ms":{},)"
        R"("was_enabled":{},"reason":"{}"}})",
        next.enabled,
        next.against_desk,
        next.max_spread_bp,
        next.max_quote_age_ms,
        next.hold_ms,
        before.enabled,
        clean);
    audit_->append({.ts = clock_->wall(),
                    .actor = request.by,
                    .action = "risk.cross.set",
                    .subject = subject,
                    .detail_json = detail});
    desk_->configure(next);
    return {};
}

std::expected<void, ControlRefusal> CrossControl::set(const GlobalPatch& patch, const ChangeRequest& request) {
    auto next = desk_->settings();
    next.enabled = patch.enabled.value_or(next.enabled);
    next.max_spread_bp = patch.max_spread_bp.value_or(next.max_spread_bp);
    next.max_quote_age_ms = patch.max_quote_age_ms.value_or(next.max_quote_age_ms);
    next.hold_ms = patch.hold_ms.value_or(next.hold_ms);
    next.against_desk = patch.against_desk.value_or(next.against_desk);
    return commit(next, "cross", request);
}

std::expected<void, ControlRefusal>
CrossControl::set_book(const std::string& name, const BookPolicy& policy, const ChangeRequest& request) {
    auto next = desk_->settings();
    next.books[name] = policy;
    return commit(next, "book:" + name, request);
}

} // namespace raikiri::internalcross
