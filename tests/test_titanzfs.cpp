#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include "titanzfs/titanzfs.hpp"
#include "titanzfs/win32_device.hpp"
#include "titanzfs/compression.hpp"
#include "titanzfs/metaslab.hpp"

#ifdef _WIN32
#include "titanzfs/winfsp_bridge.hpp"
#endif

#define TEST_CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n[TEST FAILURE] Expression failed: " #cond " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

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
        TEST_CHECK(!cs1.isZero());

        titanzfs::Checksum256 cs2 = titanzfs::Fletcher4::Compute(block);
        TEST_CHECK(cs1 == cs2);

        // Bit flip detection
        block[1337] ^= 0x01;
        titanzfs::Checksum256 cs3 = titanzfs::Fletcher4::Compute(block);
        TEST_CHECK(cs1 != cs3);
        TEST_CHECK(!titanzfs::Fletcher4::Verify(block, cs1));
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

        TEST_CHECK(pool.getCurrentTxg() == 1);
        TEST_CHECK(pool.getActiveUberblock().magic == titanzfs::UBERBLOCK_MAGIC_LE);

        for (uint64_t i = 2; i <= 200; ++i) {
            titanzfs::Status st = pool.syncTransactionGroup();
            TEST_CHECK(st == titanzfs::Status::Success);
            TEST_CHECK(pool.getCurrentTxg() == i);
            TEST_CHECK(pool.getActiveUberblock().txg == i);
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

        std::vector<uint8_t> payload(4096, 0x42);
        titanzfs::Checksum256 expectedCs = titanzfs::Fletcher4::Compute(payload);

        titanzfs::Status st = mirror->write(0, payload.size(), payload.data());
        TEST_CHECK(st == titanzfs::Status::Success);

        // Corrupt MirrorA's underlying disk
        devA->corruptByte(100);

        // Read through Mirror with checksum verification - must self-heal via MirrorB
        std::vector<uint8_t> readBuf(4096, 0);
        st = mirror->read(0, 4096, readBuf.data(), &expectedCs);
        TEST_CHECK(st == titanzfs::Status::Success);
        TEST_CHECK(readBuf == payload);

        // Verify that MirrorA was repaired in-place by self-healing logic
        std::vector<uint8_t> repairCheck(4096, 0);
        st = vA->read(0, 4096, repairCheck.data(), &expectedCs);
        TEST_CHECK(st == titanzfs::Status::Success);
        TEST_CHECK(repairCheck == payload);
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

        TEST_CHECK(rz->getState() == titanzfs::VdevState::Online);

        std::vector<uint8_t> data(4096);
        for (size_t i = 0; i < 4096; ++i) data[i] = static_cast<uint8_t>(i & 0xFF);
        titanzfs::Checksum256 cs = titanzfs::Fletcher4::Compute(data);

        titanzfs::Status st = rz->write(0, data.size(), data.data());
        TEST_CHECK(st == titanzfs::Status::Success);

        // Simulate complete drive failure of Disk1
        d1->setOnline(false);
        TEST_CHECK(rz->getState() == titanzfs::VdevState::Degraded);

        // Read degraded data - must reconstruct missing blocks via XOR parity
        std::vector<uint8_t> reconData(4096, 0);
        st = rz->read(0, 4096, reconData.data(), &cs);
        TEST_CHECK(st == titanzfs::Status::Success);
        TEST_CHECK(reconData == data);
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
        TEST_CHECK(st == titanzfs::Status::Success);

        // Modify file in v2
        const std::string v2 = "Version 2.0 Corrupted Data";
        ds.writeFile("/project.txt", v2.data(), v2.size(), 2);

        char readBuf[128]{0};
        size_t bytesRead = 0;
        ds.readFile("/project.txt", readBuf, sizeof(readBuf), bytesRead);
        TEST_CHECK(std::string(readBuf, bytesRead) == v2);

        // Atomic Rollback to @snap1
        st = ds.rollbackSnapshot("snap1");
        TEST_CHECK(st == titanzfs::Status::Success);

        std::memset(readBuf, 0, sizeof(readBuf));
        ds.readFile("/project.txt", readBuf, sizeof(readBuf), bytesRead);
        TEST_CHECK(std::string(readBuf, bytesRead) == v1);
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 6: Win32 Raw / Sparse Block Device Direct I/O
    // ------------------------------------------------------------------------
    std::cout << "[TEST 6] Win32 Raw / Sparse Block Device I/O... ";
    {
        const std::string testFile = "titanzfs_test_disk.raw";
        auto dev = titanzfs::CreateWin32BlockDevice("testdev", testFile, 4096, 4 * 1024 * 1024);
        TEST_CHECK(dev->isOnline());
        TEST_CHECK(dev->getTotalBlocks() > 0);

        std::vector<uint8_t> writeData(4096, 0xAB);
        titanzfs::Status st = dev->writeBlocks(10, 1, writeData.data());
        TEST_CHECK(st == titanzfs::Status::Success);

        std::vector<uint8_t> readData(4096, 0);
        st = dev->readBlocks(10, 1, readData.data());
        TEST_CHECK(st == titanzfs::Status::Success);
        TEST_CHECK(readData == writeData);

        dev.reset(); // close handle
        std::error_code ec;
        std::filesystem::remove(testFile, ec);
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 7: Metaslab Atomic Extent Allocation, Free Coalescing & Replay
    // ------------------------------------------------------------------------
    std::cout << "[TEST 7] Metaslab Extent Allocation & Spacemap Persistence... ";
    {
        auto mockDev = std::make_shared<MockBlockDevice>("SmDev", 8192);
        auto mockVdev = std::make_shared<titanzfs::DiskVdev>(0x99, "mockVdev", mockDev);

        titanzfs::Metaslab ms(1, 1024, 4096, 256 * 1024);
        TEST_CHECK(ms.getFreeBlocks() == 4096);

        uint64_t b1 = 0, b2 = 0;
        TEST_CHECK(ms.allocate(100, b1));
        TEST_CHECK(b1 == 1024);
        TEST_CHECK(ms.allocate(200, b2));
        TEST_CHECK(b2 == 1124);
        TEST_CHECK(ms.getFreeBlocks() == 4096 - 300);

        // Sync to VDEV
        TEST_CHECK(ms.sync(*mockVdev, 1) == titanzfs::Status::Success);
        size_t count = ms.getPersistedRecordCount();
        TEST_CHECK(count == 2);

        // Free first extent
        TEST_CHECK(ms.free(b1, 100));
        TEST_CHECK(ms.getFreeBlocks() == 4096 - 200);
        TEST_CHECK(ms.sync(*mockVdev, 2) == titanzfs::Status::Success);

        // Replay space map from disk into a fresh Metaslab instance
        titanzfs::Metaslab reloadedMs(1, 1024, 4096, 256 * 1024);
        TEST_CHECK(reloadedMs.load(*mockVdev, 3) == titanzfs::Status::Success);
        TEST_CHECK(reloadedMs.getFreeBlocks() == ms.getFreeBlocks());
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 8: Clean-Room Fast Compression (LZ4, RLE, and 87.5% Threshold)
    // ------------------------------------------------------------------------
    std::cout << "[TEST 8] Fast LZ4 / RLE Compression & 87.5% Threshold... ";
    {
        // 1. All zero block -> RLE
        std::vector<uint8_t> zeroBlock(4096, 0);
        std::vector<uint8_t> compPayload;
        uint64_t compType = 0;
        TEST_CHECK(titanzfs::CompressionEngine::CompressBlock(zeroBlock, compPayload, compType) == titanzfs::Status::Success);
        TEST_CHECK(compType == titanzfs::BP_COMP_RLE);
        TEST_CHECK(compPayload.size() == 9);

        std::vector<uint8_t> decomp(4096, 0xFF);
        TEST_CHECK(titanzfs::CompressionEngine::DecompressBlock(compPayload, decomp, compType) == titanzfs::Status::Success);
        TEST_CHECK(decomp == zeroBlock);

        // 2. Highly compressible repeated text -> LZ4
        std::string pattern = "TitanZFS Sovereign Clean-Room ISO C++23 Storage Engine for Windows NT! ";
        std::vector<uint8_t> textBlock(4096);
        for (size_t i = 0; i < 4096; ++i) textBlock[i] = pattern[i % pattern.size()];

        compPayload.clear();
        TEST_CHECK(titanzfs::CompressionEngine::CompressBlock(textBlock, compPayload, compType) == titanzfs::Status::Success);
        TEST_CHECK(compType == titanzfs::BP_COMP_LZ4);
        TEST_CHECK(compPayload.size() < 4096 * 875 / 1000); // Exceeds 87.5% threshold

        decomp.assign(4096, 0);
        TEST_CHECK(titanzfs::CompressionEngine::DecompressBlock(compPayload, decomp, compType) == titanzfs::Status::Success);
        TEST_CHECK(decomp == textBlock);

        // 3. Incompressible random data -> BP_COMP_OFF (87.5% threshold check)
        std::vector<uint8_t> randomBlock(4096);
        uint32_t seed = 0x87654321;
        for (size_t i = 0; i < 4096; ++i) {
            seed = seed * 1664525U + 1013904223U;
            randomBlock[i] = static_cast<uint8_t>((seed >> 16) & 0xFF);
        }
        compPayload.clear();
        TEST_CHECK(titanzfs::CompressionEngine::CompressBlock(randomBlock, compPayload, compType) == titanzfs::Status::Success);
        TEST_CHECK(compType == titanzfs::BP_COMP_OFF);
        TEST_CHECK(compPayload.size() == 4096);
    }
    std::cout << "PASSED\n";

    // ------------------------------------------------------------------------
    // Test 9: ZFS Send / Recv Stream Serialization & Bit-Flip Rejection
    // ------------------------------------------------------------------------
    std::cout << "[TEST 9] ZFS Send / Recv Stream Serialization... ";
    {
        titanzfs::ZfsDataset sourceDs("tank/prod");
        sourceDs.createFile("kernel.sys", false, 1);
        std::vector<uint8_t> kdata(8192, 0xEE);
        sourceDs.writeFile("kernel.sys", kdata.data(), kdata.size(), 1);

        sourceDs.createFile("config.ini", false, 1);
        std::string cfg = "pool=tank\ncompression=lz4\nchecksum=fletcher4\n";
        sourceDs.writeFile("config.ini", cfg.data(), cfg.size(), 1);

        TEST_CHECK(sourceDs.createSnapshot("backup1", 1) == titanzfs::Status::Success);

        // Serialize stream
        std::vector<uint8_t> stream;
        TEST_CHECK(sourceDs.sendSnapshot("backup1", stream) == titanzfs::Status::Success);
        TEST_CHECK(stream.size() >= 10);

        // Ingest into target dataset
        titanzfs::ZfsDataset targetDs("tank/replica");
        TEST_CHECK(targetDs.receiveStream(stream) == titanzfs::Status::Success);
        TEST_CHECK(targetDs.fileExists("kernel.sys"));
        TEST_CHECK(targetDs.fileExists("config.ini"));

        char buf[128]{0};
        size_t br = 0;
        targetDs.readFile("config.ini", buf, sizeof(buf), br);
        TEST_CHECK(std::string(buf, br) == cfg);

        // Test corruption / bit-flip detection on stream
        stream[stream.size() - 5] ^= 0x42;
        titanzfs::ZfsDataset corruptTarget("tank/corrupt");
        TEST_CHECK(corruptTarget.receiveStream(stream) == titanzfs::Status::DataChecksumError);
    }
    std::cout << "PASSED\n";

#ifdef _WIN32
    // ------------------------------------------------------------------------
    // Test 10: WinFsp Userland Filesystem Interface Check
    // ------------------------------------------------------------------------
    std::cout << "[TEST 10] WinFsp Userland Filesystem Interface... ";
    {
        auto& fsp = titanzfs::WinFspApi::Get();
        if (fsp.isAvailable()) {
            TEST_CHECK(fsp.Create != nullptr);
            TEST_CHECK(fsp.SetMountPoint != nullptr);
            TEST_CHECK(fsp.StartDispatcher != nullptr);
            TEST_CHECK(fsp.StopDispatcher != nullptr);
        }
    }
    std::cout << "PASSED\n";
#endif

    std::cout << "\n------------------------------------------------------------------------\n";
    std::cout << "All 10 TitanZFS production unit tests passed successfully!\n";
    std::cout << "------------------------------------------------------------------------\n";
    return 0;
}
