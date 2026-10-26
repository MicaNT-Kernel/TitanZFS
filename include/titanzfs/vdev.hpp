#pragma once

#include "titanzfs/types.hpp"
#include "titanzfs/fletcher4.hpp"

namespace titanzfs {

class IVdev {
public:
    virtual ~IVdev() = default;
    [[nodiscard]] virtual uint64_t getGuid() const noexcept = 0;
    [[nodiscard]] virtual const std::string& getName() const noexcept = 0;
    [[nodiscard]] virtual VdevState getState() const noexcept = 0;
    [[nodiscard]] virtual uint64_t getTotalCapacity() const noexcept = 0;
    [[nodiscard]] virtual uint64_t getAvailableCapacity() const noexcept = 0;
    [[nodiscard]] virtual uint64_t getReadErrors() const noexcept = 0;
    [[nodiscard]] virtual uint64_t getWriteErrors() const noexcept = 0;
    [[nodiscard]] virtual uint64_t getChecksumErrors() const noexcept = 0;

    virtual Status read(uint64_t offset, size_t size, void* buffer, const Checksum256* verifyChecksum = nullptr) = 0;
    virtual Status write(uint64_t offset, size_t size, const void* buffer) = 0;
};

// ============================================================================
// Disk Leaf VDEV
// ============================================================================

class DiskVdev : public IVdev {
public:
    DiskVdev(uint64_t guid, std::string name, std::shared_ptr<IBlockDevice> dev)
        : guid_(guid), name_(std::move(name)), dev_(std::move(dev)) {}

    [[nodiscard]] uint64_t getGuid() const noexcept override { return guid_; }
    [[nodiscard]] const std::string& getName() const noexcept override { return name_; }
    
    [[nodiscard]] VdevState getState() const noexcept override {
        if (!dev_ || !dev_->isOnline()) return VdevState::Faulted;
        return state_;
    }

    void setState(VdevState s) noexcept { state_ = s; }

    [[nodiscard]] uint64_t getTotalCapacity() const noexcept override {
        return dev_ ? dev_->getTotalBytes() : 0;
    }

    [[nodiscard]] uint64_t getAvailableCapacity() const noexcept override {
        return getTotalCapacity();
    }

    [[nodiscard]] uint64_t getReadErrors() const noexcept override { return readErrors_; }
    [[nodiscard]] uint64_t getWriteErrors() const noexcept override { return writeErrors_; }
    [[nodiscard]] uint64_t getChecksumErrors() const noexcept override { return checksumErrors_; }

    Status read(uint64_t offset, size_t size, void* buffer, const Checksum256* verifyChecksum = nullptr) override {
        if (!dev_ || !dev_->isOnline() || state_ == VdevState::Faulted) {
            ++readErrors_;
            return Status::NoSuchDevice;
        }

        const uint64_t bs = dev_->getBlockSize();
        const uint64_t lba = offset / bs;
        const uint64_t blocks = (size + bs - 1) / bs;

        std::vector<uint8_t> temp(blocks * bs);
        Status st = dev_->readBlocks(lba, blocks, temp.data());
        if (st != Status::Success) {
            ++readErrors_;
            return st;
        }

        const uint64_t byteOffset = offset % bs;
        std::memcpy(buffer, temp.data() + byteOffset, size);

        if (verifyChecksum && !verifyChecksum->isZero()) {
            std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(buffer), size);
            if (!Fletcher4::Verify(span, *verifyChecksum)) {
                ++checksumErrors_;
                return Status::DataChecksumError;
            }
        }

        return Status::Success;
    }

    Status write(uint64_t offset, size_t size, const void* buffer) override {
        if (!dev_ || !dev_->isOnline() || state_ == VdevState::Faulted) {
            ++writeErrors_;
            return Status::NoSuchDevice;
        }

        const uint64_t bs = dev_->getBlockSize();
        const uint64_t lba = offset / bs;
        const uint64_t blocks = (size + bs - 1) / bs;

        if (offset % bs == 0 && size == blocks * bs) {
            Status st = dev_->writeBlocks(lba, blocks, buffer);
            if (st != Status::Success) ++writeErrors_;
            return st;
        }

        std::vector<uint8_t> temp(blocks * bs);
        Status st = dev_->readBlocks(lba, blocks, temp.data());
        if (st != Status::Success) {
            ++readErrors_;
            return st;
        }

        const uint64_t byteOffset = offset % bs;
        std::memcpy(temp.data() + byteOffset, buffer, size);
        st = dev_->writeBlocks(lba, blocks, temp.data());
        if (st != Status::Success) ++writeErrors_;
        return st;
    }

private:
    uint64_t guid_{0};
    std::string name_;
    std::shared_ptr<IBlockDevice> dev_;
    VdevState state_{VdevState::Online};
    uint64_t readErrors_{0};
    uint64_t writeErrors_{0};
    uint64_t checksumErrors_{0};
};

// ============================================================================
// N-Way Mirror VDEV with Self-Healing Verification
// ============================================================================

class MirrorVdev : public IVdev {
public:
    MirrorVdev(uint64_t guid, std::string name)
        : guid_(guid), name_(std::move(name)) {}

    void addChild(std::shared_ptr<IVdev> child) {
        children_.push_back(std::move(child));
    }

    [[nodiscard]] const std::vector<std::shared_ptr<IVdev>>& getChildren() const noexcept {
        return children_;
    }

    [[nodiscard]] uint64_t getGuid() const noexcept override { return guid_; }
    [[nodiscard]] const std::string& getName() const noexcept override { return name_; }

    [[nodiscard]] VdevState getState() const noexcept override {
        size_t healthy = 0;
        for (const auto& c : children_) {
            if (c->getState() == VdevState::Online) ++healthy;
        }
        if (healthy == children_.size()) return VdevState::Online;
        if (healthy > 0) return VdevState::Degraded;
        return VdevState::Faulted;
    }

    [[nodiscard]] uint64_t getTotalCapacity() const noexcept override {
        uint64_t minCap = UINT64_MAX;
        for (const auto& c : children_) {
            minCap = std::min(minCap, c->getTotalCapacity());
        }
        return (minCap == UINT64_MAX) ? 0 : minCap;
    }

    [[nodiscard]] uint64_t getAvailableCapacity() const noexcept override {
        return getTotalCapacity();
    }

    [[nodiscard]] uint64_t getReadErrors() const noexcept override {
        uint64_t sum = 0;
        for (const auto& c : children_) sum += c->getReadErrors();
        return sum;
    }

    [[nodiscard]] uint64_t getWriteErrors() const noexcept override {
        uint64_t sum = 0;
        for (const auto& c : children_) sum += c->getWriteErrors();
        return sum;
    }

    [[nodiscard]] uint64_t getChecksumErrors() const noexcept override {
        uint64_t sum = 0;
        for (const auto& c : children_) sum += c->getChecksumErrors();
        return sum;
    }

    Status read(uint64_t offset, size_t size, void* buffer, const Checksum256* verifyChecksum = nullptr) override {
        if (children_.empty()) return Status::NoSuchDevice;

        std::vector<size_t> corruptChildren;
        bool readSuccess = false;

        for (size_t i = 0; i < children_.size(); ++i) {
            size_t idx = (readIndex_ + i) % children_.size();
            auto& child = children_[idx];

            if (child->getState() == VdevState::Faulted) continue;

            Status st = child->read(offset, size, buffer, verifyChecksum);
            if (st == Status::Success) {
                readSuccess = true;
                readIndex_ = (readIndex_ + 1) % children_.size();
                break;
            } else if (st == Status::DataChecksumError) {
                corruptChildren.push_back(idx);
            }
        }

        if (readSuccess) {
            // Self-Healing Read Repair: Write verified good data back to corrupted mirror branches
            for (size_t badIdx : corruptChildren) {
                children_[badIdx]->write(offset, size, buffer);
            }
            return Status::Success;
        }

        return Status::DataChecksumError;
    }

    Status write(uint64_t offset, size_t size, const void* buffer) override {
        if (children_.empty()) return Status::NoSuchDevice;

        size_t successCount = 0;
        for (auto& child : children_) {
            if (child->getState() == VdevState::Faulted) continue;
            if (child->write(offset, size, buffer) == Status::Success) {
                ++successCount;
            }
        }
        return (successCount > 0) ? Status::Success : Status::DeviceError;
    }

private:
    uint64_t guid_{0};
    std::string name_;
    std::vector<std::shared_ptr<IVdev>> children_;
    size_t readIndex_{0};
};

// ============================================================================
// RAID-Z1 Dynamic Single Parity VDEV (N+1 XOR Reconstruction)
// ============================================================================

class Raidz1Vdev : public IVdev {
public:
    Raidz1Vdev(uint64_t guid, std::string name)
        : guid_(guid), name_(std::move(name)) {}

    void addChild(std::shared_ptr<IVdev> child) {
        children_.push_back(std::move(child));
    }

    [[nodiscard]] const std::vector<std::shared_ptr<IVdev>>& getChildren() const noexcept {
        return children_;
    }

    [[nodiscard]] uint64_t getGuid() const noexcept override { return guid_; }
    [[nodiscard]] const std::string& getName() const noexcept override { return name_; }

    [[nodiscard]] VdevState getState() const noexcept override {
        if (children_.size() < 3) return VdevState::Faulted;
        size_t healthy = 0;
        for (const auto& c : children_) {
            if (c->getState() == VdevState::Online) ++healthy;
        }
        if (healthy == children_.size()) return VdevState::Online;
        if (healthy == children_.size() - 1) return VdevState::Degraded;
        return VdevState::Faulted;
    }

    [[nodiscard]] uint64_t getTotalCapacity() const noexcept override {
        if (children_.size() < 3) return 0;
        uint64_t minCap = UINT64_MAX;
        for (const auto& c : children_) {
            minCap = std::min(minCap, c->getTotalCapacity());
        }
        return (minCap == UINT64_MAX) ? 0 : minCap * (children_.size() - 1);
    }

    [[nodiscard]] uint64_t getAvailableCapacity() const noexcept override {
        return getTotalCapacity();
    }

    [[nodiscard]] uint64_t getReadErrors() const noexcept override {
        uint64_t sum = 0;
        for (const auto& c : children_) sum += c->getReadErrors();
        return sum;
    }

    [[nodiscard]] uint64_t getWriteErrors() const noexcept override {
        uint64_t sum = 0;
        for (const auto& c : children_) sum += c->getWriteErrors();
        return sum;
    }

    [[nodiscard]] uint64_t getChecksumErrors() const noexcept override {
        uint64_t sum = 0;
        for (const auto& c : children_) sum += c->getChecksumErrors();
        return sum;
    }

    Status read(uint64_t offset, size_t size, void* buffer, const Checksum256* verifyChecksum = nullptr) override {
        if (children_.size() < 3) return Status::NoSuchDevice;

        const size_t numDisks = children_.size();
        const size_t numDataDisks = numDisks - 1;
        const uint64_t stripeColSize = 4096;
        const uint64_t stripeIndex = offset / (stripeColSize * numDataDisks);
        const uint64_t offsetWithinStripe = offset % (stripeColSize * numDataDisks);
        const size_t dataCol = offsetWithinStripe / stripeColSize;
        const uint64_t colByteOffset = offsetWithinStripe % stripeColSize;

        const size_t parityDisk = stripeIndex % numDisks;
        const size_t targetDisk = (parityDisk + 1 + dataCol) % numDisks;
        const uint64_t diskOffset = stripeIndex * stripeColSize + colByteOffset;

        auto& targetVdev = children_[targetDisk];
        if (targetVdev->getState() == VdevState::Online) {
            Status st = targetVdev->read(diskOffset, size, buffer, verifyChecksum);
            if (st == Status::Success) return Status::Success;
        }

        // Degraded Mode: Reconstruct missing disk via XOR parity
        std::vector<uint8_t> reconBuf(size, 0);
        for (size_t d = 0; d < numDisks; ++d) {
            if (d == targetDisk) continue;
            auto& other = children_[d];
            if (other->getState() != VdevState::Online) {
                return Status::DeviceError; // Double failure - cannot reconstruct single parity
            }

            std::vector<uint8_t> blockBuf(size);
            Status st = other->read(diskOffset, size, blockBuf.data(), nullptr);
            if (st != Status::Success) return Status::DeviceError;

            for (size_t b = 0; b < size; ++b) {
                reconBuf[b] ^= blockBuf[b];
            }
        }

        if (verifyChecksum && !verifyChecksum->isZero()) {
            std::span<const uint8_t> span(reconBuf.data(), size);
            if (!Fletcher4::Verify(span, *verifyChecksum)) {
                return Status::DataChecksumError;
            }
        }

        std::memcpy(buffer, reconBuf.data(), size);
        return Status::Success;
    }

    Status write(uint64_t offset, size_t size, const void* buffer) override {
        if (children_.size() < 3) return Status::NoSuchDevice;

        const size_t numDisks = children_.size();
        const size_t numDataDisks = numDisks - 1;
        const uint64_t stripeColSize = 4096;
        const uint64_t stripeIndex = offset / (stripeColSize * numDataDisks);
        const uint64_t offsetWithinStripe = offset % (stripeColSize * numDataDisks);
        const size_t dataCol = offsetWithinStripe / stripeColSize;
        const uint64_t colByteOffset = offsetWithinStripe % stripeColSize;

        const size_t parityDisk = stripeIndex % numDisks;
        const size_t targetDisk = (parityDisk + 1 + dataCol) % numDisks;
        const uint64_t diskOffset = stripeIndex * stripeColSize + colByteOffset;

        // Read old data block
        std::vector<uint8_t> oldData(size, 0);
        children_[targetDisk]->read(diskOffset, size, oldData.data(), nullptr);

        // Read old parity block
        std::vector<uint8_t> oldParity(size, 0);
        children_[parityDisk]->read(diskOffset, size, oldParity.data(), nullptr);

        // New Parity = Old Parity ^ Old Data ^ New Data
        std::vector<uint8_t> newParity(size);
        const auto* newBytes = reinterpret_cast<const uint8_t*>(buffer);
        for (size_t i = 0; i < size; ++i) {
            newParity[i] = oldParity[i] ^ oldData[i] ^ newBytes[i];
        }

        // Write new data block
        Status stData = children_[targetDisk]->write(diskOffset, size, buffer);
        // Write new parity block
        Status stParity = children_[parityDisk]->write(diskOffset, size, newParity.data());

        return (stData == Status::Success && stParity == Status::Success) ? Status::Success : Status::DeviceError;
    }

private:
    uint64_t guid_{0};
    std::string name_;
    std::vector<std::shared_ptr<IVdev>> children_;
};

} // namespace titanzfs
