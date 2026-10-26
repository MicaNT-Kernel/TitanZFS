#pragma once

#include "titanzfs/types.hpp"
#include "titanzfs/fletcher4.hpp"
#include <vector>
#include <span>
#include <cstdint>
#include <cstring>
#include <array>

namespace titanzfs {

// ============================================================================
// Compression Type Codes for BlockPointer::props
// ============================================================================

inline constexpr uint64_t BP_COMP_MASK  = 0x00000000000000FFULL;
inline constexpr uint64_t BP_COMP_OFF   = 0;
inline constexpr uint64_t BP_COMP_LZ4   = 1;
inline constexpr uint64_t BP_COMP_RLE   = 2;

inline constexpr uint64_t BP_COMP_THRESHOLD_PERCENT = 875; // 87.5% (scaled by 10)

// Helper methods for BlockPointer compression property
inline uint64_t GetCompressionType(const BlockPointer& bp) noexcept {
    return bp.props & BP_COMP_MASK;
}

inline void SetCompressionType(BlockPointer& bp, uint64_t compType) noexcept {
    bp.props = (bp.props & ~BP_COMP_MASK) | (compType & BP_COMP_MASK);
}

// ============================================================================
// ISO C++23 LZ4 & RLE Fast Compression Engine
// ============================================================================

class CompressionEngine {
public:
    // ------------------------------------------------------------------------
    // Run-Length Encoding (Zero / Uniform Block Detection)
    // ------------------------------------------------------------------------
    static bool IsAllZero(std::span<const uint8_t> src) noexcept {
        const size_t u64Count = src.size() / sizeof(uint64_t);
        const auto* u64Ptr = reinterpret_cast<const uint64_t*>(src.data());
        for (size_t i = 0; i < u64Count; ++i) {
            if (u64Ptr[i] != 0) return false;
        }
        for (size_t i = u64Count * sizeof(uint64_t); i < src.size(); ++i) {
            if (src[i] != 0) return false;
        }
        return true;
    }

    static size_t CompressRLE(std::span<const uint8_t> src, std::vector<uint8_t>& dst) {
        dst.clear();
        if (src.empty()) return 0;

        // If the entire block is all zero, store 9 bytes: [0xFF, 8-byte length]
        if (IsAllZero(src)) {
            dst.push_back(0xFF); // All-zero marker
            const uint64_t sz = src.size();
            for (size_t i = 0; i < 8; ++i) {
                dst.push_back(static_cast<uint8_t>((sz >> (i * 8)) & 0xFF));
            }
            return dst.size();
        }

        // Simple byte-run RLE
        size_t i = 0;
        while (i < src.size()) {
            uint8_t val = src[i];
            size_t run = 1;
            while (i + run < src.size() && src[i + run] == val && run < 255) {
                ++run;
            }
            dst.push_back(static_cast<uint8_t>(run));
            dst.push_back(val);
            i += run;
        }
        return dst.size();
    }

    static bool DecompressRLE(std::span<const uint8_t> src, std::span<uint8_t> dst) noexcept {
        if (src.empty()) return true;
        if (src[0] == 0xFF && src.size() == 9) {
            std::memset(dst.data(), 0, dst.size());
            return true;
        }

        size_t srcIdx = 0;
        size_t dstIdx = 0;
        while (srcIdx + 1 < src.size() && dstIdx < dst.size()) {
            uint8_t run = src[srcIdx++];
            uint8_t val = src[srcIdx++];
            size_t toWrite = std::min(static_cast<size_t>(run), dst.size() - dstIdx);
            std::memset(dst.data() + dstIdx, val, toWrite);
            dstIdx += toWrite;
        }
        return dstIdx == dst.size();
    }

    // ------------------------------------------------------------------------
    // Standard LZ4 Block Compression
    // ------------------------------------------------------------------------
    static size_t CompressLZ4(std::span<const uint8_t> src, std::vector<uint8_t>& dst) {
        dst.clear();
        const size_t srcSize = src.size();
        if (srcSize == 0) return 0;

        // Maximum output size in worst case
        dst.reserve(srcSize + (srcSize / 255) + 16);

        constexpr size_t HASH_BITS = 12;
        constexpr size_t HASH_SIZE = 1 << HASH_BITS;
        std::array<int32_t, HASH_SIZE> hashTable{};
        hashTable.fill(-1);

        auto hash4 = [](const uint8_t* p) noexcept -> uint32_t {
            uint32_t v;
            std::memcpy(&v, p, sizeof(v));
            return (v * 2654435761U) >> (32 - HASH_BITS);
        };

        const uint8_t* const in = src.data();
        const uint8_t* ip = in;
        const uint8_t* anchor = ip;
        const uint8_t* const inEnd = in + srcSize;
        const uint8_t* const mflimit = inEnd - 12; // match finding limit

        while (ip < mflimit) {
            uint32_t h = hash4(ip);
            const uint8_t* ref = (hashTable[h] >= 0) ? (in + hashTable[h]) : nullptr;
            hashTable[h] = static_cast<int32_t>(ip - in);

            if (ref && (ip - ref <= 65535) && (ip - ref > 0)) {
                uint32_t refVal, curVal;
                std::memcpy(&refVal, ref, 4);
                std::memcpy(&curVal, ip, 4);

                if (refVal == curVal) {
                    // Match found! Count match length
                    const uint8_t* matchStart = ip;
                    size_t litLen = static_cast<size_t>(matchStart - anchor);
                    const uint8_t* match = ref + 4;
                    ip += 4;
                    while (ip < inEnd && *ip == *match) {
                        ++ip;
                        ++match;
                    }
                    size_t matchLen = static_cast<size_t>(ip - matchStart - 4);

                    // Encode token
                    uint8_t token = static_cast<uint8_t>(std::min(litLen, size_t(15)) << 4);
                    token |= static_cast<uint8_t>(std::min(matchLen, size_t(15)));
                    dst.push_back(token);

                    // Extended literal length
                    if (litLen >= 15) {
                        size_t rem = litLen - 15;
                        while (rem >= 255) {
                            dst.push_back(255);
                            rem -= 255;
                        }
                        dst.push_back(static_cast<uint8_t>(rem));
                    }

                    // Write literals
                    dst.insert(dst.end(), anchor, anchor + litLen);

                    // Write 16-bit match offset (distance back to reference)
                    uint16_t offset = static_cast<uint16_t>(matchStart - ref);
                    dst.push_back(static_cast<uint8_t>(offset & 0xFF));
                    dst.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));

                    // Extended match length
                    if (matchLen >= 15) {
                        size_t rem = matchLen - 15;
                        while (rem >= 255) {
                            dst.push_back(255);
                            rem -= 255;
                        }
                        dst.push_back(static_cast<uint8_t>(rem));
                    }

                    anchor = ip;
                    continue;
                }
            }
            ++ip;
        }

        // Encode remaining literals
        size_t lastLit = static_cast<size_t>(inEnd - anchor);
        uint8_t token = static_cast<uint8_t>(std::min(lastLit, size_t(15)) << 4);
        dst.push_back(token);
        if (lastLit >= 15) {
            size_t rem = lastLit - 15;
            while (rem >= 255) {
                dst.push_back(255);
                rem -= 255;
            }
            dst.push_back(static_cast<uint8_t>(rem));
        }
        dst.insert(dst.end(), anchor, inEnd);

        return dst.size();
    }

    // ------------------------------------------------------------------------
    // Standard LZ4 Block Decompression
    // ------------------------------------------------------------------------
    static bool DecompressLZ4(std::span<const uint8_t> src, std::span<uint8_t> dst) noexcept {
        const uint8_t* ip = src.data();
        const uint8_t* const ipEnd = ip + src.size();
        uint8_t* op = dst.data();
        uint8_t* const opEnd = op + dst.size();

        while (ip < ipEnd) {
            uint8_t token = *ip++;
            size_t litLen = token >> 4;

            if (litLen == 15) {
                while (ip < ipEnd && *ip == 255) {
                    litLen += 255;
                    ++ip;
                }
                if (ip < ipEnd) litLen += *ip++;
            }

            // Copy literals
            if (ip + litLen > ipEnd || op + litLen > opEnd) return false;
            std::memcpy(op, ip, litLen);
            ip += litLen;
            op += litLen;

            if (ip >= ipEnd) break;

            // Read offset
            if (ip + 2 > ipEnd) return false;
            uint16_t offset = static_cast<uint16_t>(ip[0] | (ip[1] << 8));
            ip += 2;
            if (offset == 0 || op - offset < dst.data()) return false;

            size_t matchLen = (token & 0x0F);
            if (matchLen == 15) {
                while (ip < ipEnd && *ip == 255) {
                    matchLen += 255;
                    ++ip;
                }
                if (ip < ipEnd) matchLen += *ip++;
            }
            matchLen += 4; // Minmatch is 4

            if (op + matchLen > opEnd) return false;

            // Copy match (supporting overlap)
            const uint8_t* match = op - offset;
            for (size_t m = 0; m < matchLen; ++m) {
                *op++ = *match++;
            }
        }
        return (op == opEnd);
    }

    // ------------------------------------------------------------------------
    // Master Block-Level Compression Dispatcher with 87.5% Threshold
    // ------------------------------------------------------------------------
    static Status CompressBlock(
        std::span<const uint8_t> uncompressed,
        std::vector<uint8_t>& compressedPayload,
        uint64_t& chosenCompType
    ) {
        if (uncompressed.empty()) {
            compressedPayload.clear();
            chosenCompType = BP_COMP_OFF;
            return Status::Success;
        }

        // 1. Try RLE for all-zero block
        if (IsAllZero(uncompressed)) {
            CompressRLE(uncompressed, compressedPayload);
            chosenCompType = BP_COMP_RLE;
            return Status::Success;
        }

        // 2. Try LZ4 compression
        std::vector<uint8_t> lz4Out;
        CompressLZ4(uncompressed, lz4Out);

        // Check 87.5% threshold: compressed * 1000 < uncompressed * 875
        const uint64_t origSize = uncompressed.size();
        const uint64_t compSize = lz4Out.size();

        if (compSize * 1000 < origSize * BP_COMP_THRESHOLD_PERCENT) {
            compressedPayload = std::move(lz4Out);
            chosenCompType = BP_COMP_LZ4;
        } else {
            // Did not achieve 87.5% threshold: store raw uncompressed
            compressedPayload.assign(uncompressed.begin(), uncompressed.end());
            chosenCompType = BP_COMP_OFF;
        }

        return Status::Success;
    }

    static Status DecompressBlock(
        std::span<const uint8_t> compressedPayload,
        std::span<uint8_t> uncompressedDest,
        uint64_t compType
    ) {
        switch (compType) {
            case BP_COMP_OFF:
                if (compressedPayload.size() != uncompressedDest.size()) {
                    return Status::BufferTooSmall;
                }
                std::memcpy(uncompressedDest.data(), compressedPayload.data(), compressedPayload.size());
                return Status::Success;

            case BP_COMP_RLE:
                if (!DecompressRLE(compressedPayload, uncompressedDest)) {
                    return Status::DataChecksumError;
                }
                return Status::Success;

            case BP_COMP_LZ4:
                if (!DecompressLZ4(compressedPayload, uncompressedDest)) {
                    return Status::DataChecksumError;
                }
                return Status::Success;

            default:
                return Status::NotImplemented;
        }
    }
};

} // namespace titanzfs
