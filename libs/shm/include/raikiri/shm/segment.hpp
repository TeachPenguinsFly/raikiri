#pragma once

// the owner is dead when its pid is gone or its heartbeat is stale
// a generation change means drop cursors and reconcile

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
inline constexpr std::uint32_t k_abi_version = 12; // 12: InstrumentBase::asset_class, underlying
// spare slots so a new channel needs no header-layout change
inline constexpr std::size_t k_max_channels = 16;
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
    // subscriptions the venue refused; shares the heartbeat's cache line
    std::atomic<std::uint64_t> subscribe_refusals{0};
    alignas(64) ChannelDesc channels[k_max_channels]{};
    ReaderSlot readers[k_max_readers]{};
};

// a laundered cast until std::start_lifetime_as ships
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

    // subscriptions refused by the venue, published beside the heartbeat
    void publish_refusals(std::uint64_t count) noexcept;

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

    // the owner's last monotonic stamp; owner_alive() decides liveness
    [[nodiscard]] std::uint64_t last_heartbeat() const noexcept {
        return header_->heartbeat_ns.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept {
        return header_->subscribe_refusals.load(std::memory_order_acquire);
    }

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
