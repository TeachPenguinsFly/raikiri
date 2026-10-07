#pragma once

// an spsc ring has one producer by construction, so a second process on the same end must be refused
// the lease is an flock on a file beside the segment; the kernel drops it when the holder exits or dies

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace raikiri::shm {

enum class LeaseRole : std::uint8_t {
    Strategy, // produces cmd_create and cmd_cancel, consumes order_intents
    Desk,     // produces order_intents
};

enum class LeaseError : std::uint8_t { Held, OpenFailed };

struct LeaseRefusal {
    LeaseError error{LeaseError::Held};
    std::int32_t holder_pid{0}; // zero when the holder has not written its pid yet
};

class WriterLease {
public:
    WriterLease(const WriterLease&) = delete;
    WriterLease& operator=(const WriterLease&) = delete;
    WriterLease(WriterLease&& other) noexcept;
    WriterLease& operator=(WriterLease&& other) noexcept;
    ~WriterLease();

    [[nodiscard]] static std::expected<WriterLease, LeaseRefusal> claim(const std::string& segment_path,
                                                                        LeaseRole role);

    void release() noexcept;

    [[nodiscard]] bool held() const noexcept { return fd_ >= 0; }

private:
    explicit WriterLease(int fd) noexcept : fd_{fd} {}

    int fd_{-1};
};

[[nodiscard]] std::string_view to_string(LeaseRole role) noexcept;
[[nodiscard]] std::string_view to_string(LeaseError error) noexcept;

// <dir>/lease.<segment>.<role>, outside the gw. prefix that readers scan for
[[nodiscard]] std::string lease_path(const std::string& segment_path, LeaseRole role);

} // namespace raikiri::shm
