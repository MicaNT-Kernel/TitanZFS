#pragma once

#include "titanzfs/types.hpp"
#include "titanzfs/vdev.hpp"
#include <map>
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <iostream>

namespace titanzfs {

// ============================================================================
// Space Map Entry (Atomic Extent Log) - 16 Bytes on-disk
// ============================================================================

#pragma pack(push, 1)
struct SpaceMapRecord {
    uint64_t offsetBlocks{0}; // LBA block offset relative to metaslab start
    uint32_t lengthBlocks{0}; // Length of the extent in blocks
    uint16_t action{0};       // 0 = ALLOCATE, 1 = FREE
    uint16_t reserved{0};
};
static_assert(sizeof(SpaceMapRecord) == 16, "SpaceMapRecord must be exactly 16 bytes");
#pragma pack(pop)

inline constexpr uint16_t SM_ALLOC = 0;
inline constexpr uint16_t SM_FREE  = 1;

// ============================================================================
// Metaslab: Manages Free Space & Extents for a VDEV Region
// ============================================================================

class Metaslab {
public:
    Metaslab(uint64_t id, uint64_t startBlock, uint64_t totalBlocks, uint64_t onDiskSmOffset)
        : id_(id), startBlock_(startBlock), totalBlocks_(totalBlocks), onDiskSmOffset_(onDiskSmOffset) {
        // Initially, the entire metaslab is one contiguous free extent
        freeTree_[startBlock_] = totalBlocks_;
        freeBlocks_ = totalBlocks_;
    }

    [[nodiscard]] uint64_t getId() const noexcept { return id_; }
    [[nodiscard]] uint64_t getStartBlock() const noexcept { return startBlock_; }
    [[nodiscard]] uint64_t getTotalBlocks() const noexcept { return totalBlocks_; }
    [[nodiscard]] uint64_t getFreeBlocks() const noexcept { return freeBlocks_; }
    [[nodiscard]] uint64_t getAllocatedBlocks() const noexcept { return totalBlocks_ - freeBlocks_; }
    [[nodiscard]] uint64_t getSpacemapDiskOffset() const noexcept { return onDiskSmOffset_; }

    // ------------------------------------------------------------------------
    // Allocate (First-Fit with Extent Splitting)
    // ------------------------------------------------------------------------
    bool allocate(uint64_t requestedBlocks, uint64_t& outBlock) {
        if (requestedBlocks == 0 || freeBlocks_ < requestedBlocks) return false;

        for (auto it = freeTree_.begin(); it != freeTree_.end(); ++it) {
            uint64_t extStart = it->first;
            uint64_t extLen = it->second;

            if (extLen >= requestedBlocks) {
                outBlock = extStart;

                // Record allocation in transaction dirty log
                SpaceMapRecord rec{
                    .offsetBlocks = outBlock - startBlock_,
                    .lengthBlocks = static_cast<uint32_t>(requestedBlocks),
                    .action = SM_ALLOC,
                    .reserved = 0
                };
                dirtyRecords_.push_back(rec);

                // Update in-memory free tree
                freeTree_.erase(it);
                if (extLen > requestedBlocks) {
                    freeTree_[extStart + requestedBlocks] = extLen - requestedBlocks;
                }
                freeBlocks_ -= requestedBlocks;
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // Free (with Extent Coalescing)
    // ------------------------------------------------------------------------
    bool free(uint64_t block, uint64_t blocksToFree) {
        if (blocksToFree == 0 || block < startBlock_ || block + blocksToFree > startBlock_ + totalBlocks_) {
            return false;
        }

        // Record free in transaction dirty log
        SpaceMapRecord rec{
            .offsetBlocks = block - startBlock_,
            .lengthBlocks = static_cast<uint32_t>(blocksToFree),
            .action = SM_FREE,
            .reserved = 0
        };
        dirtyRecords_.push_back(rec);

        // Insert and coalesce with adjacent free blocks
        uint64_t newStart = block;
        uint64_t newLen = blocksToFree;

        // Check if there is an immediately preceding extent
        auto it = freeTree_.upper_bound(block);
        if (it != freeTree_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second == block) {
                newStart = prev->first;
                newLen += prev->second;
                freeTree_.erase(prev);
            }
        }

        // Check if there is an immediately succeeding extent
        it = freeTree_.find(block + blocksToFree);
        if (it != freeTree_.end()) {
            newLen += it->second;
            freeTree_.erase(it);
        }

        freeTree_[newStart] = newLen;
        freeBlocks_ += blocksToFree;
        return true;
    }

    // ------------------------------------------------------------------------
    // Sync Space Map Log to On-Disk VDEV
    // ------------------------------------------------------------------------
    Status sync(IVdev& vdev, uint64_t currentTxg) {
        (void)currentTxg;
        if (dirtyRecords_.empty()) return Status::Success;

        // Write dirty space map records to the metaslab's dedicated log area
        const size_t bytesToWrite = dirtyRecords_.size() * sizeof(SpaceMapRecord);
        const uint64_t writeOffset = onDiskSmOffset_ + (persistedRecordCount_ * sizeof(SpaceMapRecord));

        Status st = vdev.write(writeOffset, bytesToWrite, dirtyRecords_.data());
        if (st == Status::Success) {
            persistedRecordCount_ += dirtyRecords_.size();
            dirtyRecords_.clear();
        }
        return st;
    }

    // ------------------------------------------------------------------------
    // Load and Replay Space Map Log from Disk
    // ------------------------------------------------------------------------
    Status load(IVdev& vdev, size_t recordCount) {
        // Reset in-memory free tree: assume all free initially
        freeTree_.clear();
        freeTree_[startBlock_] = totalBlocks_;
        freeBlocks_ = totalBlocks_;
        dirtyRecords_.clear();
        persistedRecordCount_ = 0;

        if (recordCount == 0) return Status::Success;

        std::vector<SpaceMapRecord> records(recordCount);
        Status st = vdev.read(onDiskSmOffset_, recordCount * sizeof(SpaceMapRecord), records.data(), nullptr);
        if (st != Status::Success) return st;

        // Replay space map records
        for (const auto& r : records) {
            const uint64_t absBlock = startBlock_ + r.offsetBlocks;
            if (r.action == SM_ALLOC) {
                // Find and split free extent
                auto it = freeTree_.upper_bound(absBlock);
                if (it != freeTree_.begin()) {
                    --it;
                    if (it->first <= absBlock && it->first + it->second >= absBlock + r.lengthBlocks) {
                        uint64_t origStart = it->first;
                        uint64_t origLen = it->second;
                        freeTree_.erase(it);

                        if (absBlock > origStart) {
                            freeTree_[origStart] = absBlock - origStart;
                        }
                        uint64_t tailStart = absBlock + r.lengthBlocks;
                        uint64_t tailLen = (origStart + origLen) - tailStart;
                        if (tailLen > 0) {
                            freeTree_[tailStart] = tailLen;
                        }
                        freeBlocks_ -= r.lengthBlocks;
                    }
                }
            } else if (r.action == SM_FREE) {
                // Replay free & coalesce
                free(absBlock, r.lengthBlocks);
                // Remove the extra record added by free() during replay
                if (!dirtyRecords_.empty()) dirtyRecords_.pop_back();
            }
        }

        persistedRecordCount_ = recordCount;
        return Status::Success;
    }

    [[nodiscard]] size_t getPersistedRecordCount() const noexcept {
        return persistedRecordCount_;
    }

    [[nodiscard]] const std::map<uint64_t, uint64_t>& getFreeTree() const noexcept {
        return freeTree_;
    }

private:
    uint64_t id_{0};
    uint64_t startBlock_{0};
    uint64_t totalBlocks_{0};
    uint64_t onDiskSmOffset_{0};
    uint64_t freeBlocks_{0};
    size_t persistedRecordCount_{0};

    std::map<uint64_t, uint64_t> freeTree_; // [startBlock -> lengthBlocks]
    std::vector<SpaceMapRecord> dirtyRecords_;
};

// ============================================================================
// Metaslab Group: Allocator across all metaslabs in a VDEV
// ============================================================================

class MetaslabGroup {
public:
    explicit MetaslabGroup(uint64_t totalBlocks, uint64_t blocksPerMetaslab = 16384) // 64MB metaslabs by default (16384 * 4KB)
        : totalBlocks_(totalBlocks), blocksPerMetaslab_(blocksPerMetaslab) {
        initializeMetaslabs();
    }

    bool allocate(uint64_t blocks, uint64_t& outBlock) {
        // Iterate through metaslabs to find one with sufficient free space
        for (auto& ms : metaslabs_) {
            if (ms->allocate(blocks, outBlock)) {
                return true;
            }
        }
        return false;
    }

    bool free(uint64_t block, uint64_t blocks) {
        for (auto& ms : metaslabs_) {
            if (block >= ms->getStartBlock() && block < ms->getStartBlock() + ms->getTotalBlocks()) {
                return ms->free(block, blocks);
            }
        }
        return false;
    }

    Status sync(IVdev& vdev, uint64_t currentTxg) {
        for (auto& ms : metaslabs_) {
            Status st = ms->sync(vdev, currentTxg);
            if (st != Status::Success) return st;
        }
        return Status::Success;
    }

    [[nodiscard]] uint64_t getTotalFreeBlocks() const noexcept {
        uint64_t freeSum = 0;
        for (const auto& ms : metaslabs_) freeSum += ms->getFreeBlocks();
        return freeSum;
    }

    [[nodiscard]] const std::vector<std::shared_ptr<Metaslab>>& getMetaslabs() const noexcept {
        return metaslabs_;
    }

private:
    void initializeMetaslabs() {
        const uint64_t metaslabCount = std::max<uint64_t>(1, totalBlocks_ / blocksPerMetaslab_);
        // Reserve the first 4MB (1024 blocks) for Uberblocks, labels, and spacemap logs
        uint64_t currentStart = 1024;
        uint64_t usablePerMetaslab = (totalBlocks_ > currentStart) ? (totalBlocks_ - currentStart) / metaslabCount : 100;

        for (uint64_t i = 0; i < metaslabCount; ++i) {
            // Dedicate 64KB log space per metaslab inside reserved label area
            uint64_t smOffset = (256 * 1024) + (i * 64 * 1024);
            metaslabs_.push_back(std::make_shared<Metaslab>(i, currentStart, usablePerMetaslab, smOffset));
            currentStart += usablePerMetaslab;
        }
    }

    uint64_t totalBlocks_{0};
    uint64_t blocksPerMetaslab_{16384};
    std::vector<std::shared_ptr<Metaslab>> metaslabs_;
};

} // namespace titanzfs
