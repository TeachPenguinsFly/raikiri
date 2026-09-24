#include <raikiri/common/platform.hpp>
#include <raikiri/shm/segment.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace raikiri::shm {

namespace {

constexpr std::size_t k_header_reserved = 4096; // header page; channels follow

struct Mapping {
    std::byte* base{nullptr};
    std::size_t size{0};
};

std::expected<Mapping, SegError> map_file(int fd, std::size_t size) {
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        return std::unexpected{SegError::MapFailed};
    }
    return Mapping{static_cast<std::byte*>(p), size};
}

// pid gone or heartbeat stale reads as dead; a false dead only marks the venue down
bool header_owner_alive(const SegmentHeader& header, std::uint64_t now_ns) {
    if (!platform::process_alive(header.creator_pid)) {
        return false;
    }
    const auto beat = header.heartbeat_ns.load(std::memory_order_acquire);
    return now_ns <= beat + k_owner_stale_ns;
}

// only a dead pid clears the path, since a hung owner may wake and keep writing
bool header_blocks_takeover(const SegmentHeader& header) {
    return platform::process_alive(header.creator_pid);
}

} // namespace

std::uint64_t layout_hash(const SegmentHeader& header) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    const auto mix = [&h](const void* data, std::size_t len) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < len; ++i) {
            h ^= bytes[i];
            h *= 0x100000001B3ULL;
        }
    };
    mix(&header.abi_version, sizeof(header.abi_version));
    mix(&header.channel_count, sizeof(header.channel_count));
    for (std::uint32_t i = 0; i < header.channel_count && i < k_max_channels; ++i) {
        const auto& ch = header.channels[i];
        mix(ch.name, sizeof(ch.name));
        mix(&ch.offset, sizeof(ch.offset));
        mix(&ch.size, sizeof(ch.size));
    }
    return h;
}

SegmentOwner::SegmentOwner(SegmentOwner&& other) noexcept
    : header_{std::exchange(other.header_, nullptr)}, base_{std::exchange(other.base_, nullptr)},
      size_{std::exchange(other.size_, 0)}, bump_{other.bump_}, path_{std::move(other.path_)} {
    other.path_.clear();
}

SegmentOwner::~SegmentOwner() {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
        ::unlink(path_.c_str());
    }
}

std::expected<SegmentOwner, SegError> SegmentOwner::create(const std::string& path,
                                                           std::size_t total_size,
                                                           std::uint64_t generation,
                                                           std::uint64_t now_ns) {
    if (total_size < k_header_reserved * 2) {
        return std::unexpected{SegError::TooSmall};
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno == EEXIST) {
            const int probe = ::open(path.c_str(), O_RDWR);
            if (probe < 0) {
                ::unlink(path.c_str());
                continue;
            }
            struct stat st{};
            bool alive = false;
            if (::fstat(probe, &st) == 0 &&
                st.st_size >= static_cast<off_t>(sizeof(SegmentHeader))) {
                if (auto m = map_file(probe, sizeof(SegmentHeader)); m.has_value()) {
                    const auto* h = channel_cast<SegmentHeader>(m->base);
                    alive = h->magic == k_magic && header_blocks_takeover(*h);
                    ::munmap(m->base, m->size);
                }
            }
            ::close(probe);
            if (alive) {
                return std::unexpected{SegError::AlreadyRunning};
            }
            ::unlink(path.c_str());
            continue;
        }
        if (fd < 0) {
            return std::unexpected{SegError::CreateFailed};
        }

        if (!platform::preallocate(fd, total_size).has_value()) {
            ::close(fd);
            ::unlink(path.c_str());
            return std::unexpected{SegError::CreateFailed};
        }
        auto mapping = map_file(fd, total_size);
        ::close(fd); // the mapping keeps the file alive
        if (!mapping.has_value()) {
            ::unlink(path.c_str());
            return std::unexpected{mapping.error()};
        }

        SegmentOwner owner;
        owner.base_ = mapping->base;
        owner.size_ = total_size;
        owner.bump_ = k_header_reserved;
        owner.path_ = path;
        owner.header_ = ::new (mapping->base) SegmentHeader{};
        owner.header_->magic = k_magic;
        owner.header_->abi_version = k_abi_version;
        owner.header_->creator_pid = ::getpid();
        owner.header_->generation = generation;
        owner.header_->heartbeat_ns.store(now_ns, std::memory_order_release);
        return owner;
    }
    return std::unexpected{SegError::CreateFailed};
}

void* SegmentOwner::place(std::string_view name, std::size_t size, std::size_t alignment) noexcept {
    if (header_ == nullptr || name.empty() || name.size() >= sizeof(ChannelDesc{}.name) ||
        header_->channel_count >= k_max_channels) {
        return nullptr;
    }
    const std::size_t aligned = (bump_ + alignment - 1) & ~(alignment - 1);
    if (aligned + size > size_) {
        return nullptr;
    }

    auto& desc = header_->channels[header_->channel_count];
    std::memcpy(desc.name, name.data(), name.size());
    desc.offset = aligned;
    desc.size = size;
    ++header_->channel_count;

    bump_ = aligned + size;
    return base_ + aligned;
}

void SegmentOwner::publish() noexcept {
    header_->layout_hash = layout_hash(*header_);
    header_->state.store(static_cast<std::uint32_t>(SegmentState::Ready),
                         std::memory_order_release);
}

void SegmentOwner::beat(std::uint64_t now_ns) noexcept {
    header_->heartbeat_ns.store(now_ns, std::memory_order_release);
}

void SegmentOwner::publish_refusals(std::uint64_t count) noexcept {
    header_->subscribe_refusals.store(count, std::memory_order_release);
}

SegmentClient::SegmentClient(SegmentClient&& other) noexcept
    : header_{std::exchange(other.header_, nullptr)}, base_{std::exchange(other.base_, nullptr)},
      size_{std::exchange(other.size_, 0)} {}

SegmentClient& SegmentClient::operator=(SegmentClient&& other) noexcept {
    if (this != &other) {
        if (base_ != nullptr) {
            ::munmap(base_, size_);
        }
        header_ = std::exchange(other.header_, nullptr);
        base_ = std::exchange(other.base_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

SegmentClient::~SegmentClient() {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
    }
}

std::expected<SegmentClient, SegError> SegmentClient::attach(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        return std::unexpected{SegError::MapFailed};
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(SegmentHeader))) {
        ::close(fd);
        return std::unexpected{SegError::TooSmall};
    }
    auto mapping = map_file(fd, static_cast<std::size_t>(st.st_size));
    ::close(fd);
    if (!mapping.has_value()) {
        return std::unexpected{mapping.error()};
    }

    SegmentClient client;
    client.base_ = mapping->base;
    client.size_ = mapping->size;
    client.header_ = channel_cast<SegmentHeader>(mapping->base);

    const auto& h = *client.header_;
    if (h.magic != k_magic) {
        return std::unexpected{SegError::BadMagic};
    }
    if (h.abi_version != k_abi_version) {
        return std::unexpected{SegError::BadAbi};
    }
    if (h.state.load(std::memory_order_acquire) !=
        static_cast<std::uint32_t>(SegmentState::Ready)) {
        return std::unexpected{SegError::NotReady};
    }
    if (h.layout_hash != layout_hash(h)) {
        return std::unexpected{SegError::BadLayout};
    }
    return client;
}

std::expected<void*, SegError> SegmentClient::find(std::string_view name,
                                                   std::size_t expected_size) noexcept {
    for (std::uint32_t i = 0; i < header_->channel_count; ++i) {
        const auto& ch = header_->channels[i];
        if (std::string_view{ch.name} == name) {
            if (ch.size != expected_size) {
                return std::unexpected{SegError::SizeMismatch};
            }
            if (ch.offset + ch.size > size_) {
                return std::unexpected{SegError::BadLayout};
            }
            return base_ + ch.offset;
        }
    }
    return std::unexpected{SegError::NoSuchChannel};
}

std::expected<std::size_t, SegError> SegmentClient::register_reader(std::int32_t pid) noexcept {
    for (std::size_t i = 0; i < k_max_readers; ++i) {
        auto& slot = header_->readers[i];
        std::int32_t cur = slot.pid.load(std::memory_order_acquire);
        const bool reclaimable = cur == 0 || !platform::process_alive(cur);
        if (reclaimable && slot.pid.compare_exchange_strong(cur, pid, std::memory_order_acq_rel)) {
            return i;
        }
    }
    return std::unexpected{SegError::NoReaderSlot};
}

void SegmentClient::beat_reader(std::size_t slot, std::uint64_t now_ns) noexcept {
    if (slot < k_max_readers) {
        header_->readers[slot].heartbeat_ns.store(now_ns, std::memory_order_release);
    }
}

bool SegmentClient::owner_alive(std::uint64_t now_ns) const noexcept {
    return header_owner_alive(*header_, now_ns);
}

} // namespace raikiri::shm
