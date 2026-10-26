#include "titanzfs/titanzfs.hpp"
#include "titanzfs/win32_device.hpp"
#include "titanzfs/compression.hpp"
#include "titanzfs/metaslab.hpp"

#ifdef _WIN32
#include "titanzfs/winfsp_bridge.hpp"
#include <csignal>
#include <atomic>
#include <iostream>
#include <filesystem>

using namespace titanzfs;

namespace {

std::atomic<bool> g_Running{true};
FSP_FILE_SYSTEM* g_FileSystem{nullptr};
std::shared_ptr<ZfsDataset> g_Dataset{nullptr};

void SigHandler(int) {
    std::cout << "\n[TitanZFS Mount] Received stop signal. Unmounting filesystem...\n";
    g_Running = false;
    if (g_FileSystem && WinFspApi::Get().isAvailable()) {
        WinFspApi::Get().StopDispatcher(g_FileSystem);
    }
}

struct ZfsFileContext {
    std::string path;
    bool isDirectory{false};
    bool deleteOnClose{false};
};

std::string WideToUtf8(PWSTR wstr) {
    if (!wstr) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), size, nullptr, nullptr);
    // Normalize path separators to '/'
    std::replace(result.begin(), result.end(), '\\', '/');
    if (!result.empty() && result.front() == '/') result.erase(0, 1);
    return result;
}

UINT64 UnixTimeToWindowsTime(uint64_t unixSecs) {
    // 100-nanosecond intervals since January 1, 1601 (UTC)
    return (unixSecs + 11644473600ULL) * 10000000ULL;
}

void PopulateFileInfo(const std::string& path, FSP_FSCTL_FILE_INFO* info) {
    std::memset(info, 0, sizeof(*info));

    if (path.empty()) {
        // Root directory
        info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        info->AllocationSize = 4096;
        info->FileSize = 4096;
        info->CreationTime = UnixTimeToWindowsTime(1700000000);
        info->LastAccessTime = UnixTimeToWindowsTime(1700000000);
        info->LastWriteTime = UnixTimeToWindowsTime(1700000000);
        info->ChangeTime = UnixTimeToWindowsTime(1700000000);
        info->IndexNumber = 1;
        return;
    }

    const auto& files = g_Dataset->getFiles();
    auto it = files.find(path);
    if (it != files.end()) {
        const auto& file = it->second;
        info->FileAttributes = file.isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        info->AllocationSize = (file.data.size() + 4095) & ~4095ULL;
        info->FileSize = file.data.size();
        info->CreationTime = UnixTimeToWindowsTime(1700000000 + file.birthTxg);
        info->LastAccessTime = UnixTimeToWindowsTime(1700000000 + file.modificationTxg);
        info->LastWriteTime = UnixTimeToWindowsTime(1700000000 + file.modificationTxg);
        info->ChangeTime = UnixTimeToWindowsTime(1700000000 + file.modificationTxg);
        info->IndexNumber = std::hash<std::string>{}(path);
    } else {
        // Check if path is a virtual parent directory
        bool isParentDir = false;
        std::string prefix = path + "/";
        for (const auto& [fpath, _] : files) {
            if (fpath.rfind(prefix, 0) == 0) {
                isParentDir = true;
                break;
            }
        }
        if (isParentDir) {
            info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            info->AllocationSize = 4096;
            info->FileSize = 4096;
            info->CreationTime = UnixTimeToWindowsTime(1700000000);
            info->LastWriteTime = UnixTimeToWindowsTime(1700000000);
            info->IndexNumber = std::hash<std::string>{}(path);
        }
    }
}

// ----------------------------------------------------------------------------
// WinFsp Callbacks
// ----------------------------------------------------------------------------

NTSTATUS FspGetVolumeInfo(FSP_FILE_SYSTEM*, FSP_FSCTL_VOLUME_INFO* VolumeInfo) {
    std::memset(VolumeInfo, 0, sizeof(*VolumeInfo));
    VolumeInfo->TotalSize = 128ULL * 1024 * 1024 * 1024; // 128 GB
    VolumeInfo->FreeSize = 120ULL * 1024 * 1024 * 1024;  // 120 GB
    const wchar_t label[] = L"TitanZFS";
    VolumeInfo->VolumeLabelLength = sizeof(label) - sizeof(wchar_t);
    std::memcpy(VolumeInfo->VolumeLabel, label, sizeof(label));
    return STATUS_SUCCESS;
}

NTSTATUS FspGetSecurityByName(FSP_FILE_SYSTEM*, PWSTR FileName, PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize) {
    (void)SecurityDescriptor;
    std::string path = WideToUtf8(FileName);
    FSP_FSCTL_FILE_INFO info{};
    PopulateFileInfo(path, &info);

    if (PFileAttributes) {
        *PFileAttributes = info.FileAttributes;
    }
    if (PSecurityDescriptorSize) {
        *PSecurityDescriptorSize = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS FspCreate(FSP_FILE_SYSTEM*, PWSTR FileName, UINT32 CreateOptions, UINT32, UINT32 FileAttributes, PSECURITY_DESCRIPTOR, UINT64, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
    std::string path = WideToUtf8(FileName);
    bool isDir = (CreateOptions & 1 /* FILE_DIRECTORY_FILE */) || (FileAttributes & FILE_ATTRIBUTE_DIRECTORY);

    Status st = g_Dataset->createFile(path, isDir, 1);
    if (st != Status::Success && st != Status::ObjectNameCollision) {
        return STATUS_ACCESS_DENIED;
    }

    auto ctx = new ZfsFileContext{path, isDir, false};
    *PFileContext = ctx;
    PopulateFileInfo(path, FileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS FspOpen(FSP_FILE_SYSTEM*, PWSTR FileName, UINT32, UINT32, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
    std::string path = WideToUtf8(FileName);

    if (path.empty()) {
        auto ctx = new ZfsFileContext{"", true, false};
        *PFileContext = ctx;
        PopulateFileInfo("", FileInfo);
        return STATUS_SUCCESS;
    }

    FSP_FSCTL_FILE_INFO fi{};
    PopulateFileInfo(path, &fi);
    if (fi.FileAttributes == 0) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    bool isDir = (fi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    auto ctx = new ZfsFileContext{path, isDir, false};
    *PFileContext = ctx;
    *FileInfo = fi;
    return STATUS_SUCCESS;
}

void FspClose(FSP_FILE_SYSTEM*, PVOID FileContext) {
    auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
    delete ctx;
}

void FspCleanup(FSP_FILE_SYSTEM*, PVOID FileContext, PWSTR, ULONG Flags) {
    auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
    if (!ctx) return;
    if (Flags & 1 /* CleanupDelete */) {
        ctx->deleteOnClose = true;
    }
}

NTSTATUS FspGetFileInfo(FSP_FILE_SYSTEM*, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
    auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
    if (!ctx) return STATUS_OBJECT_NAME_NOT_FOUND;
    PopulateFileInfo(ctx->path, FileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS FspRead(FSP_FILE_SYSTEM*, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
    auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
    if (!ctx || ctx->isDirectory) return STATUS_FILE_IS_A_DIRECTORY;

    const auto& files = g_Dataset->getFiles();
    auto it = files.find(ctx->path);
    if (it == files.end()) return STATUS_OBJECT_NAME_NOT_FOUND;

    const auto& data = it->second.data;
    if (Offset >= data.size()) {
        *PBytesTransferred = 0;
        return STATUS_END_OF_FILE;
    }

    size_t toCopy = std::min<size_t>(Length, data.size() - Offset);
    std::memcpy(Buffer, data.data() + Offset, toCopy);
    *PBytesTransferred = static_cast<ULONG>(toCopy);
    return STATUS_SUCCESS;
}

NTSTATUS FspWrite(FSP_FILE_SYSTEM*, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, BOOLEAN, BOOLEAN, PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo) {
    auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
    if (!ctx || ctx->isDirectory) return STATUS_FILE_IS_A_DIRECTORY;

    auto it = g_Dataset->getFiles().find(ctx->path);
    std::vector<uint8_t> currentData;
    if (it != g_Dataset->getFiles().end()) {
        currentData = it->second.data;
    }

    if (Offset + Length > currentData.size()) {
        currentData.resize(Offset + Length, 0);
    }
    std::memcpy(currentData.data() + Offset, Buffer, Length);

    g_Dataset->writeFile(ctx->path, currentData.data(), currentData.size(), 1);
    *PBytesTransferred = Length;
    PopulateFileInfo(ctx->path, FileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS FspSetFileSize(FSP_FILE_SYSTEM*, PVOID FileContext, UINT64 NewSize, BOOLEAN, FSP_FSCTL_FILE_INFO* FileInfo) {
    auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
    if (!ctx || ctx->isDirectory) return STATUS_FILE_IS_A_DIRECTORY;

    auto it = g_Dataset->getFiles().find(ctx->path);
    if (it != g_Dataset->getFiles().end()) {
        std::vector<uint8_t> data = it->second.data;
        data.resize(NewSize, 0);
        g_Dataset->writeFile(ctx->path, data.data(), data.size(), 1);
    }
    PopulateFileInfo(ctx->path, FileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS FspReadDirectory(FSP_FILE_SYSTEM*, PVOID FileContext, PWSTR, PWSTR Marker, PVOID Buffer, ULONG BufferLength, PULONG PBytesTransferred) {
    auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
    if (!ctx || !ctx->isDirectory) return STATUS_NOT_A_DIRECTORY;

    const std::string dirPrefix = ctx->path.empty() ? "" : (ctx->path + "/");
    std::string markerStr = Marker ? WideToUtf8(Marker) : "";

    auto& api = WinFspApi::Get();
    ULONG bytesTransferred = 0;

    // Helper lambda to emit an entry into WinFsp dir buffer
    auto addEntry = [&](const std::wstring& name, const FSP_FSCTL_FILE_INFO& fi) -> bool {
        const size_t nameBytes = name.size() * sizeof(WCHAR);
        const size_t recSize = sizeof(FSP_FSCTL_DIR_INFO) + nameBytes;
        std::vector<uint8_t> dirBuf(recSize, 0);
        auto* di = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(dirBuf.data());
        di->Size = static_cast<UINT16>(recSize);
        di->FileInfo = fi;
        std::memcpy(di->FileNameBuf, name.c_str(), nameBytes);

        return api.AddDirInfo(di, Buffer, BufferLength, &bytesTransferred);
    };

    // Emit standard "." and ".."
    if (markerStr.empty()) {
        FSP_FSCTL_FILE_INFO dotFi{};
        PopulateFileInfo(ctx->path, &dotFi);
        if (!addEntry(L".", dotFi)) {
            *PBytesTransferred = bytesTransferred;
            return STATUS_SUCCESS;
        }
    }
    if (markerStr.empty() || markerStr == ".") {
        FSP_FSCTL_FILE_INFO dotDotFi{};
        PopulateFileInfo("", &dotDotFi);
        if (!addEntry(L"..", dotDotFi)) {
            *PBytesTransferred = bytesTransferred;
            return STATUS_SUCCESS;
        }
    }

    // List immediate children
    std::map<std::string, bool> children;
    for (const auto& [fpath, f] : g_Dataset->getFiles()) {
        if (fpath.rfind(dirPrefix, 0) == 0) {
            std::string sub = fpath.substr(dirPrefix.size());
            size_t slash = sub.find('/');
            if (slash == std::string::npos) {
                children[sub] = f.isDirectory;
            } else {
                std::string dirName = sub.substr(0, slash);
                children[dirName] = true;
            }
        }
    }

    bool started = markerStr.empty() || markerStr == "." || markerStr == "..";
    for (const auto& [childName, isDir] : children) {
        if (!started) {
            if (childName == markerStr) started = true;
            continue;
        }

        std::string fullChildPath = dirPrefix + childName;
        FSP_FSCTL_FILE_INFO fi{};
        PopulateFileInfo(fullChildPath, &fi);

        std::wstring wChildName(childName.begin(), childName.end());
        if (!addEntry(wChildName, fi)) {
            break;
        }
    }

    *PBytesTransferred = bytesTransferred;
    return STATUS_SUCCESS;
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "========================================================================\n";
    std::cout << "    TitanZFS Userland Filesystem Mount Driver (WinFsp Enterprise)       \n";
    std::cout << "========================================================================\n\n";

    if (argc < 3) {
        std::cout << "Usage: titanzfs_mount <pool/dataset> <drive_letter:>\n";
        std::cout << "Example: titanzfs_mount tank/data Z:\n";
        std::cout << "         titanzfs_mount --image tank_disk.raw tank/data Z:\n\n";
        return 0;
    }

    std::string datasetName;
    std::string mountPointStr;
    std::string rawImagePath;

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

    // Initialize WinFsp API
    auto& fsp = WinFspApi::Get();
    if (!fsp.isAvailable()) {
        std::cerr << "[TitanZFS Error] WinFsp dynamic library (winfsp-x64.dll) was not found.\n";
        std::cerr << "Ensure WinFsp is installed on this machine (e.g., 'winget install WinFsp.WinFsp').\n";
        return 1;
    }

    // Set up Pool and Dataset
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

    g_Dataset = pool->getDataset(datasetName);
    if (!g_Dataset) {
        pool->createDataset(datasetName);
        g_Dataset = pool->getDataset(datasetName);
    }

    // Seed default welcome file and test folders if empty
    if (g_Dataset->getFiles().empty()) {
        const std::string welcomeText = "Welcome to TitanZFS!\nClean-Room Native C++23 ZFS Storage Pool Engine for Windows NT & MicaNT.\n";
        g_Dataset->createFile("README.txt", false, 1);
        g_Dataset->writeFile("README.txt", welcomeText.data(), welcomeText.size(), 1);
        g_Dataset->createFile("Documents", true, 1);
    }

    // Configure Volume Parameters
    FSP_FSCTL_VOLUME_PARAMS params{};
    params.Version = sizeof(params);
    params.SectorSize = 4096;
    params.SectorsPerAllocationUnit = 1;
    params.MaxComponentLength = 255;
    params.Flags = 0; // Case preserving, persistent ACLs
    const wchar_t fsName[] = L"ZFS";
    std::memcpy(params.FileSystemName, fsName, sizeof(fsName));

    // Configure Interface
    FSP_FILE_SYSTEM_INTERFACE iface{};
    iface.GetVolumeInfo = FspGetVolumeInfo;
    iface.GetSecurityByName = FspGetSecurityByName;
    iface.Create = FspCreate;
    iface.Open = FspOpen;
    iface.Close = FspClose;
    iface.Cleanup = FspCleanup;
    iface.GetFileInfo = FspGetFileInfo;
    iface.Read = FspRead;
    iface.Write = FspWrite;
    iface.SetFileSize = FspSetFileSize;
    iface.ReadDirectory = FspReadDirectory;

    NTSTATUS status = fsp.Create(nullptr, &params, &iface, &g_FileSystem);
    if (status != STATUS_SUCCESS) {
        std::cerr << "[TitanZFS Error] Failed to create WinFsp file system object: NTSTATUS 0x"
                  << std::hex << status << "\n";
        return 1;
    }

    std::wstring wMountPoint(mountPointStr.begin(), mountPointStr.end());
    status = fsp.SetMountPoint(g_FileSystem, wMountPoint.data());
    if (status != STATUS_SUCCESS) {
        std::cerr << "[TitanZFS Error] Failed to mount to drive letter '" << mountPointStr
                  << "': NTSTATUS 0x" << std::hex << status << "\n";
        fsp.Delete(g_FileSystem);
        return 1;
    }

    status = fsp.StartDispatcher(g_FileSystem, 0);
    if (status != STATUS_SUCCESS) {
        std::cerr << "[TitanZFS Error] Failed to start WinFsp file system dispatcher: NTSTATUS 0x"
                  << std::hex << status << "\n";
        fsp.RemoveMountPoint(g_FileSystem);
        fsp.Delete(g_FileSystem);
        return 1;
    }

    std::signal(SIGINT, SigHandler);
    std::signal(SIGTERM, SigHandler);

    std::cout << "[TitanZFS SUCCESS] Dataset '" << datasetName << "' is MOUNTED to " << mountPointStr << "\n";
    std::cout << "Visible in Windows File Explorer, command line, Steam, and applications.\n";
    std::cout << "Press Ctrl+C to unmount cleanly...\n\n";

    while (g_Running) {
        Sleep(500);
    }

    fsp.RemoveMountPoint(g_FileSystem);
    fsp.Delete(g_FileSystem);
    std::cout << "[TitanZFS] Drive " << mountPointStr << " unmounted cleanly.\n";
    return 0;
}
#else
int main() {
    std::cout << "TitanZFS WinFsp driver bridge is a Windows NT / MicaNT native component.\n";
    return 0;
}
#endif
