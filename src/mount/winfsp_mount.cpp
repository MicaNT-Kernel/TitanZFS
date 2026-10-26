#include "titanzfs/titanzfs.hpp"
#include "titanzfs/win32_device.hpp"

#ifdef _WIN32
#include "titanzfs/winfsp_bridge.hpp"
#include <csignal>
#include <atomic>
#include <iostream>

using namespace titanzfs;

namespace {
std::atomic<bool> g_Running{true};
WinFspMountSession g_Session;

void SigHandler(int) {
    std::cout << "\n[TitanZFS Mount] Received stop signal. Unmounting filesystem...\n";
    g_Running = false;
    g_Session.unmount();
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "========================================================================\n";
        std::cout << "    TitanZFS Userland Filesystem Mount Driver (WinFsp)                 \n";
        std::cout << "========================================================================\n\n";
        std::cout << "Usage: titanzfs_mount <pool/dataset> <drive_letter:>\n";
        std::cout << "Example: titanzfs_mount tank/data Z:\n";
        std::cout << "         titanzfs_mount --image tank_disk.raw tank/data Z:\n\n";
        return 0;
    }

    std::string rawImagePath;
    std::string datasetName;
    std::string mountPointStr;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--image" && i + 1 < argc) {
            rawImagePath = argv[++i];
        } else if (datasetName.empty()) {
            datasetName = arg;
        } else if (mountPointStr.empty()) {
            mountPointStr = arg;
        }
    }

    if (datasetName.empty() || mountPointStr.empty()) {
        std::cerr << "[TitanZFS Error] Both dataset and mount point (drive letter) must be specified.\n";
        return 1;
    }

    auto& fsp = WinFspApi::Get();
    if (!fsp.isAvailable()) {
        std::cerr << "[TitanZFS Error] WinFsp dynamic library (winfsp-x64.dll) was not found.\n";
        std::cerr << "Ensure WinFsp is installed on this machine (e.g., 'winget install WinFsp.WinFsp').\n";
        return 1;
    }

    auto& sys = TitanZfsSubsystem::Get();
    sys.loadCache();

    size_t slash = datasetName.find('/');
    std::string poolName = (slash != std::string::npos) ? datasetName.substr(0, slash) : datasetName;

    auto pool = sys.getPool(poolName);
    if (!pool) {
        std::shared_ptr<IBlockDevice> dev;
        if (!rawImagePath.empty()) {
            std::cout << "[TitanZFS] Backing pool with raw sparse block image: " << rawImagePath << "\n";
            dev = CreateWin32BlockDevice("rawdev-0", rawImagePath, 4096, 128ULL * 1024 * 1024);
        } else {
            std::cout << "[TitanZFS] Backing pool with Win32 RAM simulation device.\n";
            dev = CreateWin32BlockDevice("simdev-0", "titanzfs_backing.raw", 4096, 64ULL * 1024 * 1024);
        }

        auto vdev = std::make_shared<DiskVdev>(0x5001, "vdev-0", dev);
        pool = std::make_shared<ZfsPool>(poolName, vdev);
        pool->createDataset(datasetName);
        sys.registerPool(pool);
    }

    auto dataset = pool->getDataset(datasetName);
    if (!dataset) {
        pool->createDataset(datasetName);
        dataset = pool->getDataset(datasetName);
    }

    // Seed default welcome file if empty
    if (dataset->getFiles().empty()) {
        const std::string welcomeText = "Welcome to TitanZFS!\nNative C++23 ZFS Storage Pool Engine for Windows NT & MicaNT.\n";
        dataset->createFile("README.txt", false, 1);
        dataset->writeFile("README.txt", welcomeText.data(), welcomeText.size(), 1);
        dataset->createFile("Documents", true, 1);
    }

    std::signal(SIGINT, SigHandler);
    std::signal(SIGTERM, SigHandler);

    if (!g_Session.mount(dataset, mountPointStr)) {
        std::cerr << "[TitanZFS Error] Failed to mount dataset '" << datasetName << "' to drive '" << mountPointStr << "'\n";
        return 1;
    }

    std::cout << "[TitanZFS SUCCESS] Dataset '" << datasetName << "' is MOUNTED to " << mountPointStr << "\n";
    std::cout << "Virtual snapshot traversal active at " << mountPointStr << "\\.zfs\\snapshot\\\n";
    std::cout << "Visible in Windows File Explorer, command line, Steam, and applications.\n";
    std::cout << "Press Ctrl+C to unmount cleanly...\n\n";

    while (g_Running) {
        Sleep(500);
    }

    g_Session.unmount();
    sys.saveCache();
    std::cout << "[TitanZFS] Drive " << mountPointStr << " unmounted cleanly.\n";
    return 0;
}
#else
int main() {
    std::cout << "TitanZFS WinFsp driver bridge is a Windows NT / MicaNT native component.\n";
    return 0;
}
#endif
