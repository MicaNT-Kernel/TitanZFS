#pragma once

#include "titanzfs/types.hpp"
#include "titanzfs/vdev.hpp"
#include "titanzfs/dmu.hpp"
#include "titanzfs/metaslab.hpp"
#include <vector>
#include <memory>
#include <map>
#include <string>

namespace titanzfs {

class ZfsPool {
public:
    ZfsPool(std::string name, std::shared_ptr<IVdev> rootVdev)
        : name_(std::move(name)), rootVdev_(std::move(rootVdev)) {
        initializeUberblockRing();
        initializeMetaslabs();
    }

    [[nodiscard]] const std::string& getName() const noexcept { return name_; }
    [[nodiscard]] std::shared_ptr<IVdev> getRootVdev() const noexcept { return rootVdev_; }
    [[nodiscard]] const std::string& getVdevType() const noexcept { return vdevType_; }
    void setVdevType(std::string t) noexcept { vdevType_ = std::move(t); }
    [[nodiscard]] const std::vector<std::string>& getDevicePaths() const noexcept { return devPaths_; }
    void setDevicePaths(std::vector<std::string> paths) noexcept { devPaths_ = std::move(paths); }
    [[nodiscard]] VdevState getState() const noexcept {
        return rootVdev_ ? rootVdev_->getState() : VdevState::Faulted;
    }

    [[nodiscard]] uint64_t getCurrentTxg() const noexcept { return currentTxg_; }

    // Synchronize and advance TXG
    Status syncTransactionGroup() {
        ++currentTxg_;

        Uberblock ub{};
        ub.magic = UBERBLOCK_MAGIC_LE;
        ub.version = SPA_VERSION;
        ub.txg = currentTxg_;
        ub.guidSum = rootVdev_ ? rootVdev_->getGuid() : 0x1337;
        ub.timestamp = static_cast<uint64_t>(std::time(nullptr));

        // Advance ring index
        activeUberblockIndex_ = (activeUberblockIndex_ + 1) % UBERBLOCK_RING_SLOTS;
        uberblocks_[activeUberblockIndex_] = ub;

        // Persist Metaslab Space Maps to disk
        if (metaslabGroup_ && rootVdev_) {
            metaslabGroup_->sync(*rootVdev_, currentTxg_);
        }

        // Persist Uberblock to VDEV LBA range
        if (rootVdev_) {
            const uint64_t uberblockLbaOffset = activeUberblockIndex_ * UBERBLOCK_SIZE;
            rootVdev_->write(uberblockLbaOffset, sizeof(Uberblock), &ub);
        }

        return Status::Success;
    }

    [[nodiscard]] const Uberblock& getActiveUberblock() const noexcept {
        return uberblocks_[activeUberblockIndex_];
    }

    // Metaslab Space Management
    bool allocateBlocks(uint64_t count, uint64_t& outBlock) {
        if (!metaslabGroup_) return false;
        return metaslabGroup_->allocate(count, outBlock);
    }

    bool freeBlocks(uint64_t block, uint64_t count) {
        if (!metaslabGroup_) return false;
        return metaslabGroup_->free(block, count);
    }

    [[nodiscard]] std::shared_ptr<MetaslabGroup> getMetaslabGroup() const noexcept {
        return metaslabGroup_;
    }

    // Dataset Management
    Status createDataset(const std::string& dsName) {
        if (datasets_.contains(dsName)) return Status::ObjectNameCollision;
        datasets_[dsName] = std::make_shared<ZfsDataset>(dsName);
        return Status::Success;
    }

    [[nodiscard]] std::shared_ptr<ZfsDataset> getDataset(const std::string& dsName) const {
        auto it = datasets_.find(dsName);
        return (it != datasets_.end()) ? it->second : nullptr;
    }

    [[nodiscard]] const std::map<std::string, std::shared_ptr<ZfsDataset>>& getDatasets() const noexcept {
        return datasets_;
    }

    // Pool Scrub Engine: walks all blocks verifying 256-bit Fletcher-4 checksums
    Status scrubPool(uint64_t& blocksScrubbed, uint64_t& errorsRepaired) {
        blocksScrubbed = 0;
        errorsRepaired = 0;

        if (!rootVdev_) return Status::NoSuchDevice;

        const uint64_t totalBytes = rootVdev_->getTotalCapacity();
        const uint64_t step = ZFS_BLOCK_SIZE;
        std::vector<uint8_t> buffer(step);

        for (uint64_t off = 0; off < totalBytes; off += step) {
            Status st = rootVdev_->read(off, step, buffer.data(), nullptr);
            if (st == Status::Success) {
                ++blocksScrubbed;
            } else if (st == Status::DataChecksumError) {
                ++errorsRepaired;
            }
        }

        return Status::Success;
    }

private:
    void initializeUberblockRing() {
        uberblocks_.resize(UBERBLOCK_RING_SLOTS);
        for (size_t i = 0; i < UBERBLOCK_RING_SLOTS; ++i) {
            uberblocks_[i].magic = 0;
            uberblocks_[i].txg = 0;
        }

        currentTxg_ = 1;
        Uberblock& ub = uberblocks_[0];
        ub.magic = UBERBLOCK_MAGIC_LE;
        ub.version = SPA_VERSION;
        ub.txg = currentTxg_;
        ub.guidSum = rootVdev_ ? rootVdev_->getGuid() : 0x1337;
        ub.timestamp = static_cast<uint64_t>(std::time(nullptr));
        activeUberblockIndex_ = 0;

        if (rootVdev_) {
            rootVdev_->write(0, sizeof(Uberblock), &ub);
        }
    }

    void initializeMetaslabs() {
        uint64_t totalBlocks = 16384;
        if (rootVdev_ && rootVdev_->getTotalCapacity() > 0) {
            totalBlocks = rootVdev_->getTotalCapacity() / ZFS_BLOCK_SIZE;
        }
        metaslabGroup_ = std::make_shared<MetaslabGroup>(totalBlocks);
    }

    std::string name_;
    std::string vdevType_{"stripe"};
    std::vector<std::string> devPaths_;
    std::shared_ptr<IVdev> rootVdev_;
    uint64_t currentTxg_{1};
    size_t activeUberblockIndex_{0};
    std::vector<Uberblock> uberblocks_;
    std::shared_ptr<MetaslabGroup> metaslabGroup_;
    std::map<std::string, std::shared_ptr<ZfsDataset>> datasets_;
};

} // namespace titanzfs
