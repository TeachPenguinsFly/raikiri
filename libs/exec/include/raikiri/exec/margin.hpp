#pragma once

// leverage and margin mode on the venue; cold signed calls, made by the account poller's adapter

#include <raikiri/domain/margin.hpp>
#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::exec {

// for a venue, market or direction with no such call; serve_margin answers it as Unsupported
[[nodiscard]] VenueError margin_unsupported(std::string_view what) noexcept;

class MarginControl {
public:
    MarginControl() = default;
    MarginControl(const MarginControl&) = delete;
    MarginControl& operator=(const MarginControl&) = delete;
    virtual ~MarginControl() = default;

    [[nodiscard]] virtual std::expected<MarginState, VenueError> margin_state(InstrumentId instrument) = 0;

    // `current` is the last margin_state: a venue that sets both in one call needs the other half
    [[nodiscard]] virtual std::expected<void, VenueError>
    set_leverage(InstrumentId instrument, std::uint32_t leverage, const MarginState& current) = 0;

    [[nodiscard]] virtual std::expected<void, VenueError>
    set_margin_mode(InstrumentId instrument, MarginMode mode, const MarginState& current) = 0;
};

// reads, changes only what differs, reads again; a null control is a venue without the calls
[[nodiscard]] MarginReply serve_margin(MarginControl* control, const MarginRequest& request, UnixNanos now) noexcept;

} // namespace raikiri::exec
