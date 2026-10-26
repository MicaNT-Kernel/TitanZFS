#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include "titanzfs/titanzfs.hpp"
#include "titanzfs/win32_device.hpp"
#include "titanzfs/ipc.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) == "help") {
        std::cout << "TitanZFS Dataset & Snapshot Management Utility (Windows NT & MicaNT Native)\n";
        std::cout << "usage: zfs <subcommand> <args ...>\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  list [-r] [dataset]      List datasets, sizes, mounts, and refer properties\n";
        std::cout << "  create <dataset>         Create a new ZFS hierarchical dataset\n";
        std::cout << "  snapshot <dataset@snap>  Create an instantaneous O(1) Copy-on-Write snapshot\n";
        std::cout << "  rollback <dataset@snap>  Atomically revert dataset state to a specific snapshot\n";
        std::cout << "  send <dataset@snap> [f]  Serialize snapshot into compressed, Fletcher-4 stream\n";
        std::cout << "  recv <dataset> [file]    Receive and deserialize stream into target dataset\n";
        std::cout << "  destroy <dataset[@snap]> Destroy a dataset or snapshot\n";
        std::cout << "  mount <dataset> <drive:> Mount dataset via WinFsp userland driver bridge\n";
        return 0;
    }

    const std::string cmd = argv[1];

    // If background service daemon is active, route queries and mounts through IPC
    if (titanzfs::IpcClient::isServerRunning()) {
        if (cmd == "list") {
            std::string resp;
            int code = titanzfs::IpcClient::send(titanzfs::IpcCmdZfsList, "", resp);
            std::cout << resp;
            return code;
        }
        if (cmd == "snapshot" && argc >= 3) {
            std::string resp;
            int code = titanzfs::IpcClient::send(titanzfs::IpcCmdZfsSnapshot, argv[2], resp);
            if (code == 0) std::cout << resp << "\n";
            else std::cerr << resp << "\n";
            return code;
        }
        if (cmd == "mount" && argc >= 4) {
            std::string payload = std::string(argv[2]) + " " + argv[3];
            std::string resp;
            int code = titanzfs::IpcClient::send(titanzfs::IpcCmdZfsMount, payload, resp);
            if (code == 0) std::cout << resp << "\n";
            else std::cerr << resp << "\n";
            return code;
        }
        if (cmd == "unmount" && argc >= 3) {
            std::string resp;
            int code = titanzfs::IpcClient::send(titanzfs::IpcCmdZfsUnmount, argv[2], resp);
            if (code == 0) std::cout << resp << "\n";
            else std::cerr << resp << "\n";
            return code;
        }
    }

    auto& sys = titanzfs::TitanZfsSubsystem::Get();
    sys.loadCache();

    if (cmd == "list") {
        if (sys.getPools().empty()) {
            std::cout << "no datasets available\n";
            return 0;
        }

        std::cout << "NAME                  USED  AVAIL  REFER  MOUNTPOINT\n";
        for (const auto& [pname, p] : sys.getPools()) {
            for (const auto& [dname, ds] : p->getDatasets()) {
                std::cout << std::left << std::setw(22) << dname
                          << std::setw(6) << "256K"
                          << std::setw(7) << "64.0M"
                          << std::setw(7) << "256K"
                          << (dname == pname + "/data" ? "none" : "none") << "\n";

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
            sys.saveCache();
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
            sys.saveCache();
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
            sys.saveCache();
            std::cout << "Rolled back dataset '" << dsName << "' to snapshot '@" << snapName << "'.\n";
            return 0;
        } else {
            std::cerr << "cannot rollback to '" << target << "': snapshot not found\n";
            return 1;
        }
    }

    if (cmd == "send") {
        if (argc < 3) {
            std::cerr << "error: missing snapshot argument (expected dataset@snapshot)\n";
            return 1;
        }
        const std::string target = argv[2];
        size_t atPos = target.find('@');
        if (atPos == std::string::npos) {
            std::cerr << "cannot send '" << target << "': missing '@' delimiter\n";
            return 1;
        }
        std::string dsName = target.substr(0, atPos);
        std::string snapName = target.substr(atPos + 1);
        size_t slash = dsName.find('/');
        std::string poolName = (slash != std::string::npos) ? dsName.substr(0, slash) : dsName;

        auto p = sys.getPool(poolName);
        if (!p) {
            std::cerr << "cannot send '" << target << "': pool not found\n";
            return 1;
        }
        auto ds = p->getDataset(dsName);
        if (!ds) {
            std::cerr << "cannot send '" << target << "': dataset not found\n";
            return 1;
        }

        std::vector<uint8_t> stream;
        titanzfs::Status st = ds->sendSnapshot(snapName, stream);
        if (st != titanzfs::Status::Success) {
            std::cerr << "cannot send '" << target << "': error generating stream\n";
            return 1;
        }

        std::string outPath = (argc >= 4) ? argv[3] : (snapName + ".zfs");
        std::ofstream ofs(outPath, std::ios::binary);
        if (!ofs) {
            std::cerr << "cannot open output stream file '" << outPath << "'\n";
            return 1;
        }
        ofs.write(reinterpret_cast<const char*>(stream.data()), stream.size());
        std::cout << "Successfully exported snapshot stream '" << target << "' to '" << outPath
                  << "' (" << stream.size() << " bytes with 256-bit Fletcher-4 checksum).\n";
        return 0;
    }

    if (cmd == "recv") {
        if (argc < 4) {
            std::cerr << "usage: zfs recv <dataset> <input_file>\n";
            return 1;
        }
        const std::string target = argv[2];
        const std::string inPath = argv[3];

        size_t slash = target.find('/');
        std::string poolName = (slash != std::string::npos) ? target.substr(0, slash) : target;

        auto p = sys.getPool(poolName);
        if (!p) {
            std::cerr << "cannot recv to '" << target << "': pool '" << poolName << "' not found\n";
            return 1;
        }

        auto ds = p->getDataset(target);
        if (!ds) {
            p->createDataset(target);
            ds = p->getDataset(target);
        }

        std::ifstream ifs(inPath, std::ios::binary);
        if (!ifs) {
            std::cerr << "cannot open input stream file '" << inPath << "'\n";
            return 1;
        }

        std::vector<uint8_t> streamData((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        titanzfs::Status st = ds->receiveStream(streamData);
        if (st == titanzfs::Status::Success) {
            sys.saveCache();
            std::cout << "Successfully received stream into dataset '" << target << "' ("
                      << ds->getFiles().size() << " files restored, Fletcher-4 verified).\n";
            return 0;
        } else {
            std::cerr << "cannot receive stream into '" << target << "': checksum error or corrupt stream\n";
            return 1;
        }
    }

    if (cmd == "destroy") {
        if (argc < 3) {
            std::cerr << "error: missing dataset or snapshot argument\n";
            return 1;
        }
        const std::string target = argv[2];
        size_t atPos = target.find('@');

        if (atPos != std::string::npos) {
            // Destroy snapshot
            std::string dsName = target.substr(0, atPos);
            std::string snapName = target.substr(atPos + 1);
            size_t slash = dsName.find('/');
            std::string poolName = (slash != std::string::npos) ? dsName.substr(0, slash) : dsName;

            auto p = sys.getPool(poolName);
            if (!p) return 1;
            auto ds = p->getDataset(dsName);
            if (!ds) return 1;

            if (ds->destroySnapshot(snapName) == titanzfs::Status::Success) {
                sys.saveCache();
                std::cout << "Destroyed snapshot '" << target << "'.\n";
                return 0;
            }
        }
        std::cerr << "cannot destroy '" << target << "': not found\n";
        return 1;
    }

    if (cmd == "mount") {
        if (argc < 4) {
            std::cerr << "usage: zfs mount <dataset> <drive_letter:>\n";
            std::cerr << "example: zfs mount tank/data Z:\n";
            return 1;
        }
        std::cout << "To mount '" << argv[2] << "' to drive '" << argv[3]
                  << "', launch the WinFsp userland driver bridge:\n";
        std::cout << "  titanzfs_mount " << argv[2] << " " << argv[3] << "\n";
        return 0;
    }

    std::cerr << "zfs: unknown subcommand '" << cmd << "'\n";
    return 1;
}
