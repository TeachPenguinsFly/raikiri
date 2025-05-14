#pragma once

// Shared-memory segment lifecycle.
//
// Crash protocol:
// - The owner stamps a heartbeat every ~10 ms. Peers treat the owner as dead
//   when the pid is gone OR the heartbeat is stale (pids recycle; the
//   heartbeat is generation-scoped truth).
// - `generation` is the owner's start time: a re-created segment is a new
//   generation, and an attached consumer that observes a generation change
//   must drop cursors and reconcile (its sequence numbers belong to a dead
//   world).
// - Creation over a corpse: if the file exists but its owner is dead/stale,
//   the creator unlinks and recreates. If the owner looks alive, creation
//   fails loudly — two writers is the one unrecoverable configuration.
//
// Validation on attach: magic, ABI version, a layout hash over the channel
// directory, and per-channel byte sizes re-checked against the caller's
// compiled channel types. Struct drift between builds -> refuse to attach,
// never reinterpret garbage.

#include <raikiri/common/time.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <string>
#include <string_view>

namespace raikiri::shm {

inline constexpr std::uint64_t k_magic = 0x5241494B53484D31ULL; // "RAIKSHM1"
inline constexpr std::uint32_t k_abi_version = 2; // 2: InstrumentBase::kind
inline constexpr std::size_t k_max_channels = 8;
inline constexpr std::size_t k_max_readers = 4;
inline constexpr std::uint64_t k_owner_stale_ns = 200'000'000; // 200 ms

enum class SegError : std::uint8_t {
    CreateFailed,
    AlreadyRunning, // live owner holds this path
    MapFailed,
    TooSmall,
    BadMagic,
    BadAbi,
    BadLayout,
    NotReady,
    NoSuchChannel,
    SizeMismatch,
    NoReaderSlot,
};

enum class SegmentState : std::uint32_t { Init = 0, Ready = 1 };

struct ChannelDesc {
    char name[16]{};
    std::uint64_t offset{0};
    std::uint64_t size{0};
};

struct ReaderSlot {
    std::atomic<std::int32_t> pid{0};
    std::atomic<std::uint64_t> heartbeat_ns{0};
};

struct SegmentHeader {
    std::uint64_t magic{0};
    std::uint32_t abi_version{0};
    std::uint32_t channel_count{0};
    std::uint64_t layout_hash{0};
    std::int32_t creator_pid{0};
    std::uint32_t pad_{0};
    std::uint64_t generation{0};
    std::atomic<std::uint32_t> state{0};
    std::uint32_t pad2_{0};
    alignas(64) std::atomic<std::uint64_t> heartbeat_ns{0};
    alignas(64) ChannelDesc channels[k_max_channels]{};
    ReaderSlot readers[k_max_readers]{};
};

// C++23 gives std::start_lifetime_as for exactly this "the bytes are the
// object" situation; until libc++/libstdc++ ship it, a laundered cast is the
// universally-deployed fallback.
template <typename T> [[nodiscard]] T* channel_cast(void* p) noexcept {
#if defined(__cpp_lib_start_lifetime_as)
    return std::start_lifetime_as<T>(p);
#else
    return std::launder(reinterpret_cast<T*>(p));
#endif
}

class SegmentOwner {
public:
    SegmentOwner(const SegmentOwner&) = delete;
    SegmentOwner& operator=(const SegmentOwner&) = delete;
    SegmentOwner(SegmentOwner&& other) noexcept;
    SegmentOwner& operator=(SegmentOwner&&) = delete;
    ~SegmentOwner();

    [[nodiscard]] static std::expected<SegmentOwner, SegError> create(const std::string& path,
                                                                      std::size_t total_size,
                                                                      std::uint64_t generation,
                                                                      std::uint64_t now_ns);

    template <typename T> [[nodiscard]] T* create_channel(std::string_view name) noexcept {
        void* p = place(name, sizeof(T), alignof(T));
        return p != nullptr ? ::new (p) T{} : nullptr;
    }

    // Seals the directory, computes the layout hash, flips state to Ready.
    void publish() noexcept;

    void beat(std::uint64_t now_ns) noexcept;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    SegmentOwner() = default;

    [[nodiscard]] void*
    place(std::string_view name, std::size_t size, std::size_t alignment) noexcept;

    SegmentHeader* header_{nullptr};
    std::byte* base_{nullptr};
    std::size_t size_{0};
    std::size_t bump_{0};
    std::string path_;
};

class SegmentClient {
public:
    SegmentClient(const SegmentClient&) = delete;
    SegmentClient& operator=(const SegmentClient&) = delete;
    SegmentClient(SegmentClient&& other) noexcept;
    SegmentClient& operator=(SegmentClient&& other) noexcept;
    ~SegmentClient();

    [[nodiscard]] static std::expected<SegmentClient, SegError> attach(const std::string& path);

    // Resolves a channel; the compiled type's size must match the owner's.
    template <typename T>
    [[nodiscard]] std::expected<T*, SegError> open_channel(std::string_view name) noexcept {
        auto raw = find(name, sizeof(T));
        return raw.transform([](void* p) { return channel_cast<T>(p); });
    }

    // Claims a reader-registry slot for this process (heartbeat + liveness).
    [[nodiscard]] std::expected<std::size_t, SegError> register_reader(std::int32_t pid) noexcept;

    void beat_reader(std::size_t slot, std::uint64_t now_ns) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept { return header_->generation; }

    // Owner liveness from this consumer's point of view.
    [[nodiscard]] bool owner_alive(std::uint64_t now_ns) const noexcept;

private:
    SegmentClient() = default;

    [[nodiscard]] std::expected<void*, SegError> find(std::string_view name,
                                                      std::size_t expected_size) noexcept;

    SegmentHeader* header_{nullptr};
    std::byte* base_{nullptr};
    std::size_t size_{0};
};

// FNV-1a over the ABI version and channel directory; both sides must agree.
[[nodiscard]] std::uint64_t layout_hash(const SegmentHeader& header) noexcept;

} // namespace raikiri::shm
