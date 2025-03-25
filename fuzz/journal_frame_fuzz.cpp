// Fuzz target for journal frame decoding.
//
// The journal is our own format, which is exactly why it is worth fuzzing:
// "we wrote it, so it is well-formed" stops being true the moment a disk
// lies, a machine loses power mid-write, or an operator truncates a file.
// Recovery walks these frames on every single start — including the start
// that follows a crash — so a decoder that can be walked off the end of a
// mapping turns one bad sector into a gateway that cannot boot.
//
// The input is treated as the bytes AFTER a segment header, which is where
// frames live. Length fields, CRCs and sequence numbers are all attacker-
// controlled here, which is the point: a length that claims more bytes than
// the mapping holds is the specific bug this catches.

#include <raikiri/journal/detail.hpp>
#include <raikiri/journal/journal.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace raikiri::journal;

    // Frames begin past the header page, so prepend one. The header contents
    // do not matter to decode_frame — only that the offsets line up the way
    // a real segment's do.
    std::vector<std::byte> segment(k_header_bytes + size);
    for (std::size_t i = 0; i < size; ++i) {
        segment[k_header_bytes + i] = static_cast<std::byte>(data[i]);
    }

    FrameView frame{};
    (void)detail::decode_frame(segment.data(), segment.size(), k_header_bytes, frame);

    // The recovery path proper: walking until the first invalid frame is what
    // runs at every startup, and it must terminate on any input at all.
    (void)detail::walk_frames(segment.data(), segment.size(), /*base_record_seq=*/1);
    return 0;
}
