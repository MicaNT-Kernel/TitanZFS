#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include "titanzfs/titanzfs.hpp"

int main(int argc, char* argv[]) {
    auto& sys = titanzfs::TitanZfsSubsystem::Get();

    // Ensure default demo pool and dataset exist
    auto pool = sys.getPool("tank");
    if (!pool) {
        auto rz = std::make_shared<titanzfs::Raidz1Vdev>(0x200, "raidz1-0");
        pool = std::make_shared<titanzfs::ZfsPool>("tank", rz);
        pool->createDataset("tank/data");
        sys.registerPool(pool);
    }

    if (argc < 2 || std::string(argv[1]) == "help") {
        std::cout << "TitanZFS Dataset & Snapshot Management Utility (Windows NT & MicaNT Native)\n";
        std::cout << "usage: zfs <subcommand> <args ...>\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  list [-r] [dataset]      List datasets, sizes, mounts, and refer properties\n";
        std::cout << "  create <dataset>         Create a new ZFS hierarchical dataset\n";
        std::cout << "  snapshot <dataset@snap>  Create an instantaneous O(1) Copy-on-Write snapshot\n";
        std::cout << "  rollback <dataset@snap>  Atomically revert dataset state to a specific snapshot\n";
        std::cout << "  destroy <dataset[@snap]> Destroy a dataset or snapshot\n";
        std::cout << "  get <property> <dataset> Get dataset properties (compression, checksum, etc.)\n";
        std::cout << "  set <prop=val> <dataset> Modify dataset configuration properties\n";
        return 0;
    }

    const std::string cmd = argv[1];

    if (cmd == "list") {
        std::cout << "NAME                  USED  AVAIL  REFER  MOUNTPOINT\n";
        for (const auto& [pname, p] : sys.getPools()) {
            for (const auto& [dname, ds] : p->getDatasets()) {
                std::cout << std::left << std::setw(22) << dname
                          << std::setw(6) << "256K"
                          << std::setw(7) << "64.0M"
                          << std::setw(7) << "256K"
                          << (dname == "tank/data" ? "Z:\\" : "none") << "\n";

                // Print snapshots if any
                for (const auto& [sname, files] : ds->getSnapshots()) {
                    std::cout << std::left << std::setw(22) << (dname + "@" + sname)
                              << std::setw(6) << "0B"
                              << std::setw(7) << "-"
                              << std::setw(7) << "256K"
                              << "-\n";
                }
            }
        }
        return 0;
    }

    if (cmd == "create") {
        if (argc < 3) {
            std::cerr << "error: missing dataset argument\n";
            return 1;
        }
        const std::string target = argv[2];
        size_t slash = target.find('/');
        if (slash == std::string::npos) {
            std::cerr << "cannot create '" << target << "': invalid dataset name\n";
            return 1;
        }
        std::string poolName = target.substr(0, slash);
        auto p = sys.getPool(poolName);
        if (!p) {
            std::cerr << "cannot create '" << target << "': no such pool '" << poolName << "'\n";
            return 1;
        }
        titanzfs::Status st = p->createDataset(target);
        if (st == titanzfs::Status::Success) {
            std::cout << "Created dataset '" << target << "'.\n";
            return 0;
        } else {
            std::cerr << "cannot create '" << target << "': dataset already exists\n";
            return 1;
        }
    }

    if (cmd == "snapshot") {
        if (argc < 3) {
            std::cerr << "error: missing snapshot argument (expected dataset@snapshot)\n";
            return 1;
        }
        const std::string target = argv[2];
        size_t atPos = target.find('@');
        if (atPos == std::string::npos) {
            std::cerr << "cannot open '" << target << "': missing '@' delimiter\n";
            return 1;
        }
        std::string dsName = target.substr(0, atPos);
        std::string snapName = target.substr(atPos + 1);

        size_t slash = dsName.find('/');
        std::string poolName = (slash != std::string::npos) ? dsName.substr(0, slash) : dsName;

        auto p = sys.getPool(poolName);
        if (!p) {
            std::cerr << "cannot open '" << target << "': pool not found\n";
            return 1;
        }
        auto ds = p->getDataset(dsName);
        if (!ds) {
            std::cerr << "cannot open '" << dsName << "': dataset not found\n";
            return 1;
        }

        titanzfs::Status st = ds->createSnapshot(snapName, p->getCurrentTxg());
        if (st == titanzfs::Status::Success) {
            std::cout << "Created instantaneous snapshot '" << target << "'.\n";
            return 0;
        } else {
            std::cerr << "cannot create snapshot '" << target << "': snapshot already exists\n";
            return 1;
        }
    }

    if (cmd == "rollback") {
        if (argc < 3) {
            std::cerr << "error: missing snapshot argument\n";
            return 1;
        }
        const std::string target = argv[2];
        size_t atPos = target.find('@');
        if (atPos == std::string::npos) {
            std::cerr << "cannot rollback to '" << target << "': missing '@' delimiter\n";
            return 1;
        }
        std::string dsName = target.substr(0, atPos);
        std::string snapName = target.substr(atPos + 1);
        size_t slash = dsName.find('/');
        std::string poolName = (slash != std::string::npos) ? dsName.substr(0, slash) : dsName;

        auto p = sys.getPool(poolName);
        if (!p) {
            std::cerr << "cannot open '" << target << "': pool not found\n";
            return 1;
        }
        auto ds = p->getDataset(dsName);
        if (!ds) {
            std::cerr << "cannot open '" << dsName << "': dataset not found\n";
            return 1;
        }

        titanzfs::Status st = ds->rollbackSnapshot(snapName);
        if (st == titanzfs::Status::Success) {
            std::cout << "Rolled back dataset '" << dsName << "' to snapshot '@" << snapName << "'.\n";
            return 0;
        } else {
            std::cerr << "cannot rollback to '" << target << "': snapshot not found\n";
            return 1;
        }
    }

    std::cerr << "zfs: unknown subcommand '" << cmd << "'\n";
    return 1;
}
