#pragma once

#include "titanzfs/types.hpp"

namespace titanzfs {

class Fletcher4 {
public:
    // Pure ISO C++23 Fletcher-4 Checksum Engine (4x 64-bit accumulators over 32-bit words)
    static Checksum256 Compute(std::span<const uint8_t> data) noexcept {
        uint64_t a = 0;
        uint64_t b = 0;
        uint64_t c = 0;
        uint64_t d = 0;

        const size_t wordCount = data.size() / sizeof(uint32_t);
        const auto* words = reinterpret_cast<const uint32_t*>(data.data());

        for (size_t i = 0; i < wordCount; ++i) {
            a += words[i];
            b += a;
            c += b;
            d += c;
        }

        // Handle remaining tail bytes if not 4-byte aligned
        const size_t remainder = data.size() % sizeof(uint32_t);
        if (remainder > 0) {
            uint32_t tail = 0;
            const uint8_t* tailPtr = data.data() + (wordCount * sizeof(uint32_t));
            for (size_t i = 0; i < remainder; ++i) {
                tail |= static_cast<uint32_t>(tailPtr[i]) << (i * 8);
            }
            a += tail;
            b += a;
            c += b;
            d += c;
        }

        Checksum256 cs;
        cs.words[0] = a;
        cs.words[1] = b;
        cs.words[2] = c;
        cs.words[3] = d;
        return cs;
    }

    static bool Verify(std::span<const uint8_t> data, const Checksum256& expected) noexcept {
        const Checksum256 actual = Compute(data);
        return actual == expected;
    }
};

} // namespace titanzfs
