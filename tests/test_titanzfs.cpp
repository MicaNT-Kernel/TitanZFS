#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>
#include "titanzfs/titanzfs.hpp"

// In-Memory Block Device for testing
class MockBlockDevice : public titanzfs::IBlockDevice {
public:
    MockBlockDevice(std::string name, uint64_t totalBlocks, uint64_t blockSize = 4096)
        : name_(std::move(name)), totalBlocks_(totalBlocks), blockSize_(blockSize), storage_(totalBlocks * blockSize, 0) {}

    [[nodiscard]] uint64_t getBlockSize() const noexcept override { return blockSize_; }
    [[nodiscard]] uint64_t getTotalBlocks() const noexcept override { return totalBlocks_; }

    titanzfs::Status readBlocks(uint64_t lba, uint64_t count, void* buffer) override {
        if (!online_) return titanzfs::Status::NoSuchDevice;
        if (lba + count > totalBlocks_) return titanzfs::Status::InvalidParameter;
        std::memcpy(buffer, storage_.data() + (lba * blockSize_), count * blockSize_);
        return titanzfs::Status::Success;
    }

    titanzfs::Status writeBlocks(uint64_t lba, uint64_t count, const void* buffer) override {
        if (!online_) return titanzfs::Status::NoSuchDevice;
        if (lba + count > totalBlocks_) return titanzfs::Status::InvalidParameter;
        std::memcpy(storage_.data() + (lba * blockSize_), buffer, count * blockSize_);
        return titanzfs::Status::Success;
    }

    [[nodiscard]] bool isOnline() const noexcept override { return online_; }
    void setOnline(bool online) noexcept override { online_ = online; }
    [[nodiscard]] const std::string& getDeviceName() const noexcept override { return name_; }

    void corruptByte(uint64_t offset, uint8_t xorMask = 0xFF) {
        if (offset < storage_.size()) {
            storage_[offset] ^= xorMask;
        }
    }

private:
    std::string name_;
    uint64_t totalBlocks_;
    uint64_t blockSize_;
    bool online_{true};
    std::vector<uint8_t> storage_;
};

int main() {
    std::cout << "========================================================================\n";
    std::cout << "        TitanZFS Native C++23 Standalone Test Suite                     \n";
    std::cout << "========================================================================\n\n";

    // ------------------------------------------------------------------------
    // Test 1: Fletcher-4 256-bit Checksum Engine & Bit-Flip Sensitivity
    // ------------------------------------------------------------------------
    std::cout << "[TEST 1] Fletcher-4 256-Bit Checksumming... ";
    {
        std::vector<uint8_t> block(4096, 0x5A);
        titanzfs::Checksum256 cs1 = titanzfs::Fletcher4::Compute(block);
        assert(!cs1.isZero());

        // Verify repeatability
        titanzfs::Checksum256 cs2 = titanzfs::Fletcher4::Compute(block);
        assert(cs1 == cs2);

        // Bit flip detection
        block[1337] ^= 0x01;
        titanzfs::Checksum256 cs3 = titanzfs::Fletcher4::Compute(block);
        assert(cs1 != cs3);
        assert(!titanzfs::Fletcher4::Verify(block, cs1));
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 2: Uberblock Ring & Monotonic TXG Progression
    // ------------------------------------------------------------------------
    std::cout << "[TEST 2] 128-Slot Uberblock Ring & Monotonic TXG... ";
    {
        auto dev = std::make_shared<MockBlockDevice>("UberblockDev", 1024);
        auto vdev = std::make_shared<titanzfs::DiskVdev>(0x01, "vdev0", dev);
        titanzfs::ZfsPool pool("testpool", vdev);

        assert(pool.getCurrentTxg() == 1);
        assert(pool.getActiveUberblock().magic == titanzfs::UBERBLOCK_MAGIC_LE);

        for (uint64_t i = 2; i <= 200; ++i) {
            titanzfs::Status st = pool.syncTransactionGroup();
            assert(st == titanzfs::Status::Success);
            assert(pool.getCurrentTxg() == i);
            assert(pool.getActiveUberblock().txg == i);
        }
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 3: Mirror VDEV & Transparent Self-Healing Read Failover
    // ------------------------------------------------------------------------
    std::cout << "[TEST 3] Mirror VDEV & Transparent Read Self-Healing... ";
    {
        auto devA = std::make_shared<MockBlockDevice>("MirrorA", 2048);
        auto devB = std::make_shared<MockBlockDevice>("MirrorB", 2048);

        auto vA = std::make_shared<titanzfs::DiskVdev>(0x10, "dA", devA);
        auto vB = std::make_shared<titanzfs::DiskVdev>(0x20, "dB", devB);

        auto mirror = std::make_shared<titanzfs::MirrorVdev>(0x30, "mirror-0");
        mirror->addChild(vA);
        mirror->addChild(vB);

        // Write known data
        std::vector<uint8_t> payload(4096, 0x42);
        titanzfs::Checksum256 expectedCs = titanzfs::Fletcher4::Compute(payload);

        titanzfs::Status st = mirror->write(0, payload.size(), payload.data());
        assert(st == titanzfs::Status::Success);

        // Corrupt MirrorA's underlying disk
        devA->corruptByte(100);

        // Read through Mirror with checksum verification - must self-heal via MirrorB
        std::vector<uint8_t> readBuf(4096, 0);
        st = mirror->read(0, 4096, readBuf.data(), &expectedCs);
        assert(st == titanzfs::Status::Success);
        assert(readBuf == payload);

        // Verify that MirrorA was repaired in-place by self-healing logic
        std::vector<uint8_t> repairCheck(4096, 0);
        st = vA->read(0, 4096, repairCheck.data(), &expectedCs);
        assert(st == titanzfs::Status::Success);
        assert(repairCheck == payload);
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 4: RAID-Z1 Dynamic Single Parity & XOR Degraded Reconstruction
    // ------------------------------------------------------------------------
    std::cout << "[TEST 4] RAID-Z1 Single Parity & XOR Rebuild... ";
    {
        auto d0 = std::make_shared<MockBlockDevice>("Disk0", 4096);
        auto d1 = std::make_shared<MockBlockDevice>("Disk1", 4096);
        auto d2 = std::make_shared<MockBlockDevice>("Disk2", 4096);

        auto v0 = std::make_shared<titanzfs::DiskVdev>(0x101, "v0", d0);
        auto v1 = std::make_shared<titanzfs::DiskVdev>(0x102, "v1", d1);
        auto v2 = std::make_shared<titanzfs::DiskVdev>(0x103, "v2", d2);

        auto rz = std::make_shared<titanzfs::Raidz1Vdev>(0x200, "raidz1-0");
        rz->addChild(v0);
        rz->addChild(v1);
        rz->addChild(v2);

        assert(rz->getState() == titanzfs::VdevState::Online);

        // Write structured data
        std::vector<uint8_t> data(4096);
        for (size_t i = 0; i < 4096; ++i) data[i] = static_cast<uint8_t>(i & 0xFF);
        titanzfs::Checksum256 cs = titanzfs::Fletcher4::Compute(data);

        titanzfs::Status st = rz->write(0, data.size(), data.data());
        assert(st == titanzfs::Status::Success);

        // Simulate complete drive failure of Disk1
        d1->setOnline(false);
        assert(rz->getState() == titanzfs::VdevState::Degraded);

        // Read degraded data - must reconstruct missing blocks via XOR parity
        std::vector<uint8_t> reconData(4096, 0);
        st = rz->read(0, 4096, reconData.data(), &cs);
        assert(st == titanzfs::Status::Success);
        assert(reconData == data);
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 5: DMU Datasets, CoW Snapshots & Atomic Rollbacks
    // ------------------------------------------------------------------------
    std::cout << "[TEST 5] DMU Datasets, CoW Snapshots & Rollbacks... ";
    {
        titanzfs::ZfsDataset ds("tank/work");
        ds.createFile("/project.txt", false, 1);

        const std::string v1 = "Version 1.0 Initial Data";
        ds.writeFile("/project.txt", v1.data(), v1.size(), 1);

        // Snapshot @snap1
        titanzfs::Status st = ds.createSnapshot("snap1", 1);
        assert(st == titanzfs::Status::Success);

        // Modify file in v2
        const std::string v2 = "Version 2.0 Corrupted Data";
        ds.writeFile("/project.txt", v2.data(), v2.size(), 2);

        char readBuf[128]{0};
        size_t bytesRead = 0;
        ds.readFile("/project.txt", readBuf, sizeof(readBuf), bytesRead);
        assert(std::string(readBuf, bytesRead) == v2);

        // Atomic Rollback to @snap1
        st = ds.rollbackSnapshot("snap1");
        assert(st == titanzfs::Status::Success);

        std::memset(readBuf, 0, sizeof(readBuf));
        ds.readFile("/project.txt", readBuf, sizeof(readBuf), bytesRead);
        assert(std::string(readBuf, bytesRead) == v1);
    }
    std::cout << "PASSED\n";

    std::cout << "\n------------------------------------------------------------------------\n";
    std::cout << "All TitanZFS standalone unit tests passed successfully!\n";
    std::cout << "------------------------------------------------------------------------\n";
    return 0;
}
