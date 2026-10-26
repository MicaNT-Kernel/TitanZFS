#pragma once

#include "titanzfs/types.hpp"
#include "titanzfs/fletcher4.hpp"
#include "titanzfs/vdev.hpp"
#include "titanzfs/dmu.hpp"
#include "titanzfs/spa.hpp"
#include "titanzfs/win32_device.hpp"
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace titanzfs {

// ============================================================================
// Global Pool Registry (Subsystem Singleton) & Persistent Cache
// ============================================================================

class TitanZfsSubsystem {
public:
    static TitanZfsSubsystem& Get() noexcept {
        static TitanZfsSubsystem instance;
        return instance;
    }

    static std::string getDefaultCachePath() {
        const char* envPath = std::getenv("TITANZFS_CACHE");
        if (envPath && envPath[0] != '\0') {
            return std::string(envPath);
        }
        return "zpool.cache";
    }

    Status registerPool(std::shared_ptr<ZfsPool> pool) {
        if (!pool) return Status::InvalidParameter;
        if (pools_.contains(pool->getName())) return Status::ObjectNameCollision;
        pools_[pool->getName()] = std::move(pool);
        return Status::Success;
    }

    [[nodiscard]] std::shared_ptr<ZfsPool> getPool(const std::string& name) const {
        auto it = pools_.find(name);
        return (it != pools_.end()) ? it->second : nullptr;
    }

    [[nodiscard]] const std::map<std::string, std::shared_ptr<ZfsPool>>& getPools() const noexcept {
        return pools_;
    }

    Status destroyPool(const std::string& name) {
        auto it = pools_.find(name);
        if (it == pools_.end()) return Status::ObjectNameNotFound;
        pools_.erase(it);
        if (pools_.empty()) {
            std::remove(getDefaultCachePath().c_str());
        } else {
            saveCache();
        }
        return Status::Success;
    }

    bool saveCache(const std::string& cachePath = "") {
        const std::string path = cachePath.empty() ? getDefaultCachePath() : cachePath;
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;

        const uint64_t magic = 0x5A504F4F4C434831ULL; // "ZPOOLCH1"
        ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

        const uint32_t poolCount = static_cast<uint32_t>(pools_.size());
        ofs.write(reinterpret_cast<const char*>(&poolCount), sizeof(poolCount));

        for (const auto& [pname, pool] : pools_) {
            uint32_t nameLen = static_cast<uint32_t>(pname.size());
            ofs.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
            ofs.write(pname.data(), nameLen);

            std::string vtype = pool->getVdevType();
            uint32_t vtypeLen = static_cast<uint32_t>(vtype.size());
            ofs.write(reinterpret_cast<const char*>(&vtypeLen), sizeof(vtypeLen));
            ofs.write(vtype.data(), vtypeLen);

            const auto& devPaths = pool->getDevicePaths();
            uint32_t devCount = static_cast<uint32_t>(devPaths.size());
            ofs.write(reinterpret_cast<const char*>(&devCount), sizeof(devCount));
            for (const auto& p : devPaths) {
                uint32_t pLen = static_cast<uint32_t>(p.size());
                ofs.write(reinterpret_cast<const char*>(&pLen), sizeof(pLen));
                ofs.write(p.data(), pLen);
            }

            uint64_t curTxg = pool->getCurrentTxg();
            ofs.write(reinterpret_cast<const char*>(&curTxg), sizeof(curTxg));

            const auto& datasets = pool->getDatasets();
            uint32_t dsCount = static_cast<uint32_t>(datasets.size());
            ofs.write(reinterpret_cast<const char*>(&dsCount), sizeof(dsCount));

            for (const auto& [dsName, ds] : datasets) {
                uint32_t dsNameLen = static_cast<uint32_t>(dsName.size());
                ofs.write(reinterpret_cast<const char*>(&dsNameLen), sizeof(dsNameLen));
                ofs.write(dsName.data(), dsNameLen);

                std::vector<uint8_t> activeStream;
                ds->sendSnapshot("$ACTIVE$", activeStream);
                uint64_t actSize = activeStream.size();
                ofs.write(reinterpret_cast<const char*>(&actSize), sizeof(actSize));
                if (actSize > 0) {
                    ofs.write(reinterpret_cast<const char*>(activeStream.data()), actSize);
                }

                const auto& snaps = ds->getSnapshots();
                uint32_t snapCount = static_cast<uint32_t>(snaps.size());
                ofs.write(reinterpret_cast<const char*>(&snapCount), sizeof(snapCount));

                for (const auto& [snapName, _] : snaps) {
                    uint32_t sLen = static_cast<uint32_t>(snapName.size());
                    ofs.write(reinterpret_cast<const char*>(&sLen), sizeof(sLen));
                    ofs.write(snapName.data(), sLen);

                    std::vector<uint8_t> snapStream;
                    ds->sendSnapshot(snapName, snapStream);
                    uint64_t sSize = snapStream.size();
                    ofs.write(reinterpret_cast<const char*>(&sSize), sizeof(sSize));
                    if (sSize > 0) {
                        ofs.write(reinterpret_cast<const char*>(snapStream.data()), sSize);
                    }
                }
            }
        }
        return true;
    }

    bool loadCache(const std::string& cachePath = "") {
        const std::string path = cachePath.empty() ? getDefaultCachePath() : cachePath;
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return false;

        uint64_t magic = 0;
        ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != 0x5A504F4F4C434831ULL) return false;

        uint32_t poolCount = 0;
        ifs.read(reinterpret_cast<char*>(&poolCount), sizeof(poolCount));

        for (uint32_t p = 0; p < poolCount; ++p) {
            uint32_t nameLen = 0;
            ifs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
            std::string pname(nameLen, '\0');
            ifs.read(pname.data(), nameLen);

            uint32_t vtypeLen = 0;
            ifs.read(reinterpret_cast<char*>(&vtypeLen), sizeof(vtypeLen));
            std::string vtype(vtypeLen, '\0');
            ifs.read(vtype.data(), vtypeLen);

            uint32_t devCount = 0;
            ifs.read(reinterpret_cast<char*>(&devCount), sizeof(devCount));
            std::vector<std::string> devPaths;
            std::vector<std::shared_ptr<IVdev>> childVdevs;

            for (uint32_t d = 0; d < devCount; ++d) {
                uint32_t pLen = 0;
                ifs.read(reinterpret_cast<char*>(&pLen), sizeof(pLen));
                std::string dpath(pLen, '\0');
                ifs.read(dpath.data(), pLen);
                devPaths.push_back(dpath);

                auto blkDev = CreateWin32BlockDevice(dpath, dpath, 4096, 64 * 1024 * 1024);
                childVdevs.push_back(std::make_shared<DiskVdev>(0x500 + d, dpath, blkDev));
            }

            std::shared_ptr<IVdev> rootVdev;
            if (vtype == "raidz1" && childVdevs.size() >= 3) {
                auto rz = std::make_shared<Raidz1Vdev>(0x300, "raidz1-0");
                for (auto& c : childVdevs) rz->addChild(c);
                rootVdev = rz;
            } else if (vtype == "mirror" && !childVdevs.empty()) {
                auto mir = std::make_shared<MirrorVdev>(0x200, "mirror-0");
                for (auto& c : childVdevs) mir->addChild(c);
                rootVdev = mir;
            } else if (!childVdevs.empty()) {
                rootVdev = childVdevs[0];
            }

            auto pool = std::make_shared<ZfsPool>(pname, rootVdev);
            pool->setVdevType(vtype);
            pool->setDevicePaths(devPaths);

            uint64_t curTxg = 0;
            ifs.read(reinterpret_cast<char*>(&curTxg), sizeof(curTxg));

            uint32_t dsCount = 0;
            ifs.read(reinterpret_cast<char*>(&dsCount), sizeof(dsCount));

            for (uint32_t dsIdx = 0; dsIdx < dsCount; ++dsIdx) {
                uint32_t dsNameLen = 0;
                ifs.read(reinterpret_cast<char*>(&dsNameLen), sizeof(dsNameLen));
                std::string dsName(dsNameLen, '\0');
                ifs.read(dsName.data(), dsNameLen);

                pool->createDataset(dsName);
                auto ds = pool->getDataset(dsName);

                uint64_t actSize = 0;
                ifs.read(reinterpret_cast<char*>(&actSize), sizeof(actSize));
                if (actSize > 0) {
                    std::vector<uint8_t> actBuf(actSize);
                    ifs.read(reinterpret_cast<char*>(actBuf.data()), actSize);
                    if (ds) ds->receiveStream(actBuf);
                }

                uint32_t snapCount = 0;
                ifs.read(reinterpret_cast<char*>(&snapCount), sizeof(snapCount));
                for (uint32_t s = 0; s < snapCount; ++s) {
                    uint32_t sLen = 0;
                    ifs.read(reinterpret_cast<char*>(&sLen), sizeof(sLen));
                    std::string sName(sLen, '\0');
                    ifs.read(sName.data(), sLen);

                    uint64_t sSize = 0;
                    ifs.read(reinterpret_cast<char*>(&sSize), sizeof(sSize));
                    std::vector<uint8_t> sBuf(sSize);
                    if (sSize > 0) {
                        ifs.read(reinterpret_cast<char*>(sBuf.data()), sSize);
                    }
                    if (ds) {
                        ds->receiveSnapshotStream(sName, curTxg, sBuf);
                    }
                }
            }

            pools_[pname] = pool;
        }
        return true;
    }

private:
    TitanZfsSubsystem() = default;
    std::map<std::string, std::shared_ptr<ZfsPool>> pools_;
};

} // namespace titanzfs
