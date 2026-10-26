#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <algorithm>
#include <chrono>

namespace titanzfs {

// ============================================================================
// Constants & Architectural Signatures
// ============================================================================

inline constexpr uint64_t UBERBLOCK_MAGIC   = 0x00BAB10C00BAB10CULL; // Canonical ZFS Uberblock magic (Big/Little endian clean)
inline constexpr uint64_t UBERBLOCK_MAGIC_LE= 0x00BAB10C;
inline constexpr uint64_t SPA_VERSION       = 5000;                  // Feature flags / Sovereign clean-room NT
inline constexpr size_t   UBERBLOCK_SIZE    = 1024;                  // Standard 1KB Uberblock
inline constexpr size_t   UBERBLOCK_RING_SLOTS = 128;               // 128-slot cyclical ring
inline constexpr size_t   ZFS_BLOCK_SIZE    = 4096;                  // Default 4KB records
inline constexpr size_t   SPA_MAXBLOCKSIZE  = 16 * 1024 * 1024;      // 16MB maximum record

// ============================================================================
// Status Codes (freestanding & NT compatible)
// ============================================================================

enum class Status : uint32_t {
    Success               = 0x00000000,
    NoSuchDevice          = 0xC000000E,
    AccessDenied          = 0xC0000022,
    BufferTooSmall        = 0xC0000023,
    ObjectNameNotFound    = 0xC0000034,
    ObjectNameCollision   = 0xC0000035,
    DataChecksumError     = 0xC00002EB,
    DeviceError           = 0xC000009C,
    DiskFull              = 0xC000007F,
    VolumeNotMounted      = 0xC00000A1,
    MediaWriteProtected   = 0xC00000A2,
    InvalidParameter      = 0xC000000D,
    NotImplemented        = 0xC0000002
};

// ============================================================================
// VDEV Topologies & Health States
// ============================================================================

enum class VdevState : uint32_t {
    Unknown  = 0,
    Closed   = 1,
    Offline  = 2,
    Removed  = 3,
    CantOpen = 4,
    Faulted  = 5,
    Degraded = 6,
    Online   = 7
};

inline const char* ToString(VdevState state) noexcept {
    switch (state) {
        case VdevState::Online:   return "ONLINE";
        case VdevState::Degraded: return "DEGRADED";
        case VdevState::Faulted:  return "FAULTED";
        case VdevState::Offline:  return "OFFLINE";
        case VdevState::Removed:  return "REMOVED";
        case VdevState::CantOpen: return "UNAVAIL";
        case VdevState::Closed:   return "CLOSED";
        default:                  return "UNKNOWN";
    }
}

// ============================================================================
// 256-Bit Checksum Container (Fletcher-4)
// ============================================================================

struct alignas(8) Checksum256 {
    uint64_t words[4]{ 0, 0, 0, 0 };

    constexpr bool operator==(const Checksum256& other) const noexcept {
        return words[0] == other.words[0] &&
               words[1] == other.words[1] &&
               words[2] == other.words[2] &&
               words[3] == other.words[3];
    }

    constexpr bool operator!=(const Checksum256& other) const noexcept {
        return !(*this == other);
    }

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return (words[0] | words[1] | words[2] | words[3]) == 0;
    }
};

#pragma pack(push, 1)

// ============================================================================
// Data Virtual Address (DVA) - 16 bytes
// ============================================================================

struct Dva {
    uint64_t asize_grid_vdev{0}; // [63]=gang, [62:56]=grid, [55:32]=asize, [31:0]=vdev (8B)
    uint64_t offset{0};          // offset within vdev (8B)

    [[nodiscard]] constexpr uint32_t getVdev() const noexcept {
        return static_cast<uint32_t>(asize_grid_vdev & 0xFFFFFFFFULL);
    }
    [[nodiscard]] constexpr uint32_t getAsize() const noexcept {
        return static_cast<uint32_t>((asize_grid_vdev >> 32) & 0xFFFFFFULL);
    }
    constexpr void set(uint32_t vdev, uint32_t asize, uint64_t off) noexcept {
        asize_grid_vdev = (static_cast<uint64_t>(asize & 0xFFFFFFULL) << 32) | (vdev & 0xFFFFFFFFULL);
        offset = off;
    }
};
static_assert(sizeof(Dva) == 16, "Dva must be exactly 16 bytes");

// ============================================================================
// Block Pointer (blkptr_t) - 128 Bytes
// ============================================================================

struct BlockPointer {
    Dva         dva[3]{};        // Up to 3 redundant physical locations (48B)
    uint64_t    props{0};        // Comp, Type, Encryption, Checksum type (8B)
    uint64_t    padding[3]{0};   // Reserved/Padded (24B)
    uint64_t    birthTxg{0};     // Transaction Group when committed (8B)
    uint64_t    fillCount{0};    // Number of non-zero children blocks (8B)
    Checksum256 checksum{};      // 256-bit Fletcher-4 Checksum (32B)
    // Total size: 48 + 8 + 24 + 8 + 8 + 32 = 128 bytes
};
static_assert(sizeof(BlockPointer) == 128, "BlockPointer must be exactly 128 bytes");

// ============================================================================
// Uberblock (uberblock_t) - 1024 Bytes
// ============================================================================

struct Uberblock {
    uint64_t     magic{0};       // 0x00BAB10C (8B)
    uint64_t     version{0};     // 5000 (Sovereign Clean-Room) (8B)
    uint64_t     txg{0};         // Monotonically increasing Transaction Group (8B)
    uint64_t     guidSum{0};     // XOR sum of all participating top-level VDEVs (8B)
    uint64_t     timestamp{0};   // Epoch seconds (8B)
    BlockPointer rootBp{};       // 128-byte Root Object Block Pointer (128B)
    uint64_t     checkpointTxg{0}; // Checkpoint TXG (8B)
    uint8_t      reserved[1024 - 48 - sizeof(BlockPointer)]{0}; // Pad to 1024 bytes (848B)
};
static_assert(sizeof(Uberblock) == 1024, "Uberblock must be exactly 1024 bytes");

#pragma pack(pop)

// ============================================================================
// Abstract Block Device Interface
// ============================================================================

class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;
    [[nodiscard]] virtual uint64_t getBlockSize() const noexcept = 0;
    [[nodiscard]] virtual uint64_t getTotalBlocks() const noexcept = 0;
    [[nodiscard]] virtual uint64_t getTotalBytes() const noexcept {
        return getBlockSize() * getTotalBlocks();
    }
    virtual Status readBlocks(uint64_t lba, uint64_t count, void* buffer) = 0;
    virtual Status writeBlocks(uint64_t lba, uint64_t count, const void* buffer) = 0;
    [[nodiscard]] virtual bool isOnline() const noexcept = 0;
    virtual void setOnline(bool online) noexcept = 0;
    [[nodiscard]] virtual const std::string& getDeviceName() const noexcept = 0;
};

} // namespace titanzfs
