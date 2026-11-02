#include <raikiri/audit/queue.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>

namespace raikiri::audit {

EntryQueue::EntryQueue(std::size_t slots, std::size_t slot_bytes)
    : mask_{std::bit_ceil(std::max<std::size_t>(slots, 2)) - 1}, slot_bytes_{std::max(slot_bytes, k_min_slot_bytes)},
      headers_{std::make_unique<Header[]>(mask_ + 1)}, bytes_{std::make_unique<char[]>((mask_ + 1) * slot_bytes_)} {
    for (std::size_t i = 0; i <= mask_; ++i) {
        headers_[i].seq.store(i, std::memory_order_relaxed);
    }
}

bool EntryQueue::try_push(const Entry& entry, bool& truncated) noexcept {
    std::uint64_t pos = enqueue_.load(std::memory_order_relaxed);
    Header* header = nullptr;
    while (true) {
        header = &headers_[pos & mask_];
        const auto seq = header->seq.load(std::memory_order_acquire);
        const auto ahead = static_cast<std::int64_t>(seq - pos);
        if (ahead == 0) {
            if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (ahead < 0) {
            return false;
        } else {
            pos = enqueue_.load(std::memory_order_relaxed);
        }
    }

    char* out = bytes_.get() + ((pos & mask_) * slot_bytes_);
    const auto label = [&out, &truncated](std::string_view text) {
        const auto kept = std::min(text.size(), k_max_label_bytes);
        truncated = truncated || kept != text.size();
        std::memcpy(out, text.data(), kept);
        out += kept;
        return static_cast<std::uint16_t>(kept);
    };
    truncated = false;
    header->ts = entry.ts.ns;
    header->actor = label(entry.actor);
    header->action = label(entry.action);
    header->subject = label(entry.subject);
    const auto room = static_cast<std::size_t>(bytes_.get() + ((pos & mask_) * slot_bytes_) + slot_bytes_ - out);
    if (entry.detail_json.size() <= room) {
        std::memcpy(out, entry.detail_json.data(), entry.detail_json.size());
        header->detail = static_cast<std::uint32_t>(entry.detail_json.size());
    } else {
        truncated = true;
        constexpr std::string_view k_head = "{\"truncated\":true,\"bytes\":";
        char* cursor = std::copy(k_head.begin(), k_head.end(), out);
        cursor = std::to_chars(cursor, cursor + 24, entry.detail_json.size()).ptr;
        *cursor++ = '}';
        header->detail = static_cast<std::uint32_t>(cursor - out);
    }
    header->seq.store(pos + 1, std::memory_order_release);
    return true;
}

bool EntryQueue::peek(Entry& entry) const noexcept {
    const Header& header = headers_[dequeue_ & mask_];
    if (header.seq.load(std::memory_order_acquire) != dequeue_ + 1) {
        return false;
    }
    const char* in = bytes_.get() + ((dequeue_ & mask_) * slot_bytes_);
    const auto take = [&in](std::size_t size) {
        const std::string_view text{in, size};
        in += size;
        return text;
    };
    entry.ts = {header.ts};
    entry.actor = take(header.actor);
    entry.action = take(header.action);
    entry.subject = take(header.subject);
    entry.detail_json = take(header.detail);
    return true;
}

void EntryQueue::consume() noexcept {
    headers_[dequeue_ & mask_].seq.store(dequeue_ + mask_ + 1, std::memory_order_release);
    ++dequeue_;
}

} // namespace raikiri::audit
