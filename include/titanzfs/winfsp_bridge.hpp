#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <iostream>
#include <memory>
#include <cstring>
#include "titanzfs/types.hpp"
#include "titanzfs/dmu.hpp"

namespace titanzfs {

// ============================================================================
// NTSTATUS Codes
// ============================================================================

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_OBJECT_NAME_NOT_FOUND     ((NTSTATUS)0xC0000034L)
#define STATUS_OBJECT_NAME_COLLISION     ((NTSTATUS)0xC0000035L)
#define STATUS_ACCESS_DENIED             ((NTSTATUS)0xC0000022L)
#define STATUS_END_OF_FILE               ((NTSTATUS)0xC0000011L)
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023L)
#define STATUS_MEDIA_WRITE_PROTECTED     ((NTSTATUS)0xC00000A2L)
#define STATUS_FILE_IS_A_DIRECTORY       ((NTSTATUS)0xC00000BAL)
#define STATUS_NOT_A_DIRECTORY           ((NTSTATUS)0xC0000103L)
#endif

#pragma pack(push, 1)

struct FSP_FSCTL_VOLUME_INFO {
    UINT64 TotalSize;
    UINT64 FreeSize;
    UINT16 VolumeLabelLength;
    WCHAR  VolumeLabel[32];
};

struct FSP_FSCTL_FILE_INFO {
    UINT32 FileAttributes;
    UINT32 ReparseTag;
    UINT64 AllocationSize;
    UINT64 FileSize;
    UINT64 CreationTime;
    UINT64 LastAccessTime;
    UINT64 LastWriteTime;
    UINT64 ChangeTime;
    UINT64 IndexNumber;
    UINT32 HardLinks;
    UINT32 EaSize;
};

struct FSP_FSCTL_DIR_INFO {
    UINT16 Size;
    FSP_FSCTL_FILE_INFO FileInfo;
    union {
        UINT64 NextOffset;
        UINT8  Padding[24];
    };
    WCHAR  FileNameBuf[1];
};

struct FSP_FSCTL_VOLUME_PARAMS {
    UINT16 Version;
    UINT16 SectorSize;
    UINT16 SectorsPerAllocationUnit;
    UINT16 MaxComponentLength;
    UINT64 VolumeCreationTime;
    UINT32 VolumeSerialNumber;
    UINT32 TransactTimeout;
    UINT32 IrpTimeout;
    UINT32 IrpCapacity;
    UINT32 FileInfoTimeout;
    UINT32 Flags;
    WCHAR  FileSystemName[16];
    UINT8  Reserved[432]; // 72 + 432 = 504 bytes
};
static_assert(sizeof(FSP_FSCTL_VOLUME_PARAMS) == 504, "FSP_FSCTL_VOLUME_PARAMS must be exactly 504 bytes");

#pragma pack(pop)

struct _FSP_FILE_SYSTEM {
    PVOID UserContext;
};
typedef struct _FSP_FILE_SYSTEM FSP_FILE_SYSTEM;

struct FSP_FILE_SYSTEM_INTERFACE {
    NTSTATUS (*GetVolumeInfo)(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_VOLUME_INFO *VolumeInfo);
    NTSTATUS (*SetVolumeLabel)(FSP_FILE_SYSTEM *FileSystem, PWSTR VolumeLabel, FSP_FSCTL_VOLUME_INFO *VolumeInfo);
    NTSTATUS (*GetSecurityByName)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize);
    NTSTATUS (*Create)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize, PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*Open)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess, PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*Overwrite)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize, FSP_FSCTL_FILE_INFO *FileInfo);
    void     (*Cleanup)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags);
    void     (*Close)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext);
    NTSTATUS (*Read)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, PULONG PBytesTransferred);
    NTSTATUS (*Write)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo, PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*Flush)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*GetFileInfo)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*SetInformation)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo, ULONG FileInfoClass, PVOID FileInfoBuffer, ULONG FileInfoSize);
    NTSTATUS (*SetFileSize)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO *FileInfo);
    NTSTATUS (*CanDelete)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName);
    NTSTATUS (*Rename)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists);
    NTSTATUS (*GetSecurity)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize);
    NTSTATUS (*SetSecurity)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor);
    NTSTATUS (*ReadDirectory)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR Pattern, PWSTR Marker, PVOID Buffer, ULONG BufferLength, PULONG PBytesTransferred);
    NTSTATUS (*ResolveReparsePoints)(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 ReparsePointIndex, BOOLEAN ResolveLastPathComponent, PVOID PIoStatus, PVOID Buffer, PSIZE_T PSize);
    NTSTATUS (*GetEa)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, ULONG Length, PULONG PBytesTransferred);
    NTSTATUS (*SetEa)(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, ULONG Length, FSP_FSCTL_FILE_INFO *FileInfo);
};

// ============================================================================
// WinFsp Dynamic API Function Pointer Table
// ============================================================================

class WinFspApi {
public:
    using FnFspFileSystemCreate = NTSTATUS (*)(PWSTR DevicePath, const FSP_FSCTL_VOLUME_PARAMS *VolumeParams, const FSP_FILE_SYSTEM_INTERFACE *Interface, FSP_FILE_SYSTEM **PFileSystem);
    using FnFspFileSystemDelete = void (*)(FSP_FILE_SYSTEM *FileSystem);
    using FnFspFileSystemSetMountPoint = NTSTATUS (*)(FSP_FILE_SYSTEM *FileSystem, PWSTR MountPoint);
    using FnFspFileSystemRemoveMountPoint = NTSTATUS (*)(FSP_FILE_SYSTEM *FileSystem);
    using FnFspFileSystemStartDispatcher = NTSTATUS (*)(FSP_FILE_SYSTEM *FileSystem, ULONG ThreadCount);
    using FnFspFileSystemStopDispatcher = void (*)(FSP_FILE_SYSTEM *FileSystem);
    using FnFspFileSystemAddDirInfo = BOOLEAN (*)(FSP_FSCTL_DIR_INFO *DirInfo, PVOID Buffer, ULONG BufferLength, PULONG PBytesTransferred);

    static WinFspApi& Get() {
        static WinFspApi instance;
        return instance;
    }

    [[nodiscard]] bool isAvailable() const noexcept {
        return hModule_ != nullptr;
    }

    FnFspFileSystemCreate           Create{nullptr};
    FnFspFileSystemDelete           Delete{nullptr};
    FnFspFileSystemSetMountPoint    SetMountPoint{nullptr};
    FnFspFileSystemRemoveMountPoint RemoveMountPoint{nullptr};
    FnFspFileSystemStartDispatcher  StartDispatcher{nullptr};
    FnFspFileSystemStopDispatcher   StopDispatcher{nullptr};
    FnFspFileSystemAddDirInfo       AddDirInfo{nullptr};

private:
    WinFspApi() {
        loadDll();
    }

    ~WinFspApi() {
        if (hModule_) {
            FreeLibrary(hModule_);
            hModule_ = nullptr;
        }
    }

    void loadDll() {
        hModule_ = LoadLibraryW(L"winfsp-x64.dll");
        if (!hModule_) {
            hModule_ = LoadLibraryW(L"C:\\Program Files (x86)\\WinFsp\\bin\\winfsp-x64.dll");
        }
        if (!hModule_) {
            hModule_ = LoadLibraryW(L"C:\\Program Files\\WinFsp\\bin\\winfsp-x64.dll");
        }

        if (hModule_) {
            Create = reinterpret_cast<FnFspFileSystemCreate>(GetProcAddress(hModule_, "FspFileSystemCreate"));
            Delete = reinterpret_cast<FnFspFileSystemDelete>(GetProcAddress(hModule_, "FspFileSystemDelete"));
            SetMountPoint = reinterpret_cast<FnFspFileSystemSetMountPoint>(GetProcAddress(hModule_, "FspFileSystemSetMountPoint"));
            RemoveMountPoint = reinterpret_cast<FnFspFileSystemRemoveMountPoint>(GetProcAddress(hModule_, "FspFileSystemRemoveMountPoint"));
            StartDispatcher = reinterpret_cast<FnFspFileSystemStartDispatcher>(GetProcAddress(hModule_, "FspFileSystemStartDispatcher"));
            StopDispatcher = reinterpret_cast<FnFspFileSystemStopDispatcher>(GetProcAddress(hModule_, "FspFileSystemStopDispatcher"));
            AddDirInfo = reinterpret_cast<FnFspFileSystemAddDirInfo>(GetProcAddress(hModule_, "FspFileSystemAddDirInfo"));
        }
    }

    HMODULE hModule_{nullptr};
};

inline std::string WideToUtf8(PWSTR wstr) {
    if (!wstr) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), size, nullptr, nullptr);
    std::replace(result.begin(), result.end(), '\\', '/');
    if (!result.empty() && result.front() == '/') result.erase(0, 1);
    return result;
}

inline UINT64 UnixTimeToWindowsTime(uint64_t unixSecs) {
    return (unixSecs + 11644473600ULL) * 10000000ULL;
}

struct ZfsFileContext {
    std::string path;
    bool isDirectory{false};
    bool deleteOnClose{false};
    bool isSnapshot{false};
};

// ============================================================================
// WinFspMountSession: Encapsulated Dataset Drive Mount with .zfs/snapshot
// ============================================================================

class WinFspMountSession {
public:
    WinFspMountSession() = default;
    ~WinFspMountSession() { unmount(); }

    bool mount(std::shared_ptr<ZfsDataset> dataset, const std::string& mountPoint) {
        if (!dataset || mountPoint.empty()) return false;
        if (mounted_) return true;

        auto& fsp = WinFspApi::Get();
        if (!fsp.isAvailable()) return false;

        dataset_ = std::move(dataset);
        mountPoint_ = mountPoint;

        FSP_FSCTL_VOLUME_PARAMS params{};
        params.Version = sizeof(params);
        params.SectorSize = 4096;
        params.SectorsPerAllocationUnit = 1;
        params.MaxComponentLength = 255;
        params.Flags = 0;
        const wchar_t fsName[] = L"ZFS";
        std::memcpy(params.FileSystemName, fsName, sizeof(fsName));

        FSP_FILE_SYSTEM_INTERFACE iface{};
        iface.GetVolumeInfo = &WinFspMountSession::CbGetVolumeInfo;
        iface.GetSecurityByName = &WinFspMountSession::CbGetSecurityByName;
        iface.Create = &WinFspMountSession::CbCreate;
        iface.Open = &WinFspMountSession::CbOpen;
        iface.Close = &WinFspMountSession::CbClose;
        iface.Cleanup = &WinFspMountSession::CbCleanup;
        iface.GetFileInfo = &WinFspMountSession::CbGetFileInfo;
        iface.Read = &WinFspMountSession::CbRead;
        iface.Write = &WinFspMountSession::CbWrite;
        iface.SetFileSize = &WinFspMountSession::CbSetFileSize;
        iface.ReadDirectory = &WinFspMountSession::CbReadDirectory;

        NTSTATUS status = fsp.Create(nullptr, &params, &iface, &fileSystem_);
        if (status != STATUS_SUCCESS || !fileSystem_) {
            return false;
        }

        fileSystem_->UserContext = this;

        std::wstring wMountPoint(mountPoint_.begin(), mountPoint_.end());
        status = fsp.SetMountPoint(fileSystem_, wMountPoint.data());
        if (status != STATUS_SUCCESS) {
            fsp.Delete(fileSystem_);
            fileSystem_ = nullptr;
            return false;
        }

        status = fsp.StartDispatcher(fileSystem_, 0);
        if (status != STATUS_SUCCESS) {
            fsp.RemoveMountPoint(fileSystem_);
            fsp.Delete(fileSystem_);
            fileSystem_ = nullptr;
            return false;
        }

        mounted_ = true;
        return true;
    }

    void unmount() {
        if (!mounted_ || !fileSystem_) return;
        auto& fsp = WinFspApi::Get();
        if (fsp.isAvailable()) {
            fsp.StopDispatcher(fileSystem_);
            fsp.RemoveMountPoint(fileSystem_);
            fsp.Delete(fileSystem_);
        }
        fileSystem_ = nullptr;
        mounted_ = false;
    }

    [[nodiscard]] bool isMounted() const noexcept { return mounted_; }
    [[nodiscard]] const std::string& getMountPoint() const noexcept { return mountPoint_; }
    [[nodiscard]] std::shared_ptr<ZfsDataset> getDataset() const noexcept { return dataset_; }
    void setDataset(std::shared_ptr<ZfsDataset> ds) noexcept { dataset_ = std::move(ds); }

    void populateFileInfo(const std::string& path, FSP_FSCTL_FILE_INFO* info) const {
        std::memset(info, 0, sizeof(*info));

        if (path.empty()) {
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

        // Virtual .zfs snapshot filesystem tree
        if (path == ".zfs" || path == ".zfs/snapshot") {
            info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
            info->AllocationSize = 4096;
            info->FileSize = 4096;
            info->CreationTime = UnixTimeToWindowsTime(1700000000);
            info->LastWriteTime = UnixTimeToWindowsTime(1700000000);
            info->IndexNumber = std::hash<std::string>{}(path);
            return;
        }

        if (path.rfind(".zfs/snapshot/", 0) == 0 && dataset_) {
            std::string sub = path.substr(14);
            size_t slash = sub.find('/');
            std::string snapName = (slash == std::string::npos) ? sub : sub.substr(0, slash);

            const auto& snaps = dataset_->getSnapshots();
            auto snapIt = snaps.find(snapName);
            if (snapIt == snaps.end()) return;

            if (slash == std::string::npos) {
                // Snapshot root folder
                info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
                info->AllocationSize = 4096;
                info->FileSize = 4096;
                info->CreationTime = UnixTimeToWindowsTime(1700000000);
                info->LastWriteTime = UnixTimeToWindowsTime(1700000000);
                info->IndexNumber = std::hash<std::string>{}(path);
                return;
            }

            std::string relPath = sub.substr(slash + 1);
            const auto& snapFiles = snapIt->second;
            auto fit = snapFiles.find(relPath);
            if (fit != snapFiles.end()) {
                const auto& file = fit->second;
                info->FileAttributes = (file.isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_READONLY;
                info->AllocationSize = (file.data.size() + 4095) & ~4095ULL;
                info->FileSize = file.data.size();
                info->CreationTime = UnixTimeToWindowsTime(1700000000 + file.birthTxg);
                info->LastAccessTime = UnixTimeToWindowsTime(1700000000 + file.modificationTxg);
                info->LastWriteTime = UnixTimeToWindowsTime(1700000000 + file.modificationTxg);
                info->ChangeTime = UnixTimeToWindowsTime(1700000000 + file.modificationTxg);
                info->IndexNumber = std::hash<std::string>{}(path);
                return;
            }

            // Virtual directory inside snapshot
            bool isParentDir = false;
            std::string prefix = relPath + "/";
            for (const auto& [fpath, _] : snapFiles) {
                if (fpath.rfind(prefix, 0) == 0) {
                    isParentDir = true;
                    break;
                }
            }
            if (isParentDir) {
                info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
                info->AllocationSize = 4096;
                info->FileSize = 4096;
                info->CreationTime = UnixTimeToWindowsTime(1700000000);
                info->LastWriteTime = UnixTimeToWindowsTime(1700000000);
                info->IndexNumber = std::hash<std::string>{}(path);
                return;
            }
            return;
        }

        // Active dataset files
        if (!dataset_) return;
        const auto& files = dataset_->getFiles();
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

private:
    static WinFspMountSession* FromFs(FSP_FILE_SYSTEM* fs) {
        return fs ? reinterpret_cast<WinFspMountSession*>(fs->UserContext) : nullptr;
    }

    static NTSTATUS CbGetVolumeInfo(FSP_FILE_SYSTEM* FileSystem, FSP_FSCTL_VOLUME_INFO* VolumeInfo) {
        auto* self = FromFs(FileSystem);
        if (!self) return STATUS_ACCESS_DENIED;

        std::memset(VolumeInfo, 0, sizeof(*VolumeInfo));
        VolumeInfo->TotalSize = 128ULL * 1024 * 1024 * 1024;
        VolumeInfo->FreeSize = 120ULL * 1024 * 1024 * 1024;
        const wchar_t label[] = L"TitanZFS";
        VolumeInfo->VolumeLabelLength = sizeof(label) - sizeof(wchar_t);
        std::memcpy(VolumeInfo->VolumeLabel, label, sizeof(label));
        return STATUS_SUCCESS;
    }

    static NTSTATUS CbGetSecurityByName(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize) {
        (void)SecurityDescriptor;
        auto* self = FromFs(FileSystem);
        if (!self) return STATUS_ACCESS_DENIED;

        std::string path = WideToUtf8(FileName);
        FSP_FSCTL_FILE_INFO info{};
        self->populateFileInfo(path, &info);

        if (PFileAttributes) *PFileAttributes = info.FileAttributes;
        if (PSecurityDescriptorSize) *PSecurityDescriptorSize = 0;
        return STATUS_SUCCESS;
    }

    static NTSTATUS CbCreate(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32 CreateOptions, UINT32, UINT32 FileAttributes, PSECURITY_DESCRIPTOR, UINT64, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
        auto* self = FromFs(FileSystem);
        if (!self || !self->dataset_) return STATUS_ACCESS_DENIED;

        std::string path = WideToUtf8(FileName);
        if (path.rfind(".zfs", 0) == 0) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }

        bool isDir = (CreateOptions & 1 /* FILE_DIRECTORY_FILE */) || (FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        Status st = self->dataset_->createFile(path, isDir, 1);
        if (st != Status::Success && st != Status::ObjectNameCollision) {
            return STATUS_ACCESS_DENIED;
        }

        auto ctx = new ZfsFileContext{path, isDir, false, false};
        *PFileContext = ctx;
        self->populateFileInfo(path, FileInfo);
        return STATUS_SUCCESS;
    }

    static NTSTATUS CbOpen(FSP_FILE_SYSTEM* FileSystem, PWSTR FileName, UINT32, UINT32, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
        auto* self = FromFs(FileSystem);
        if (!self) return STATUS_ACCESS_DENIED;

        std::string path = WideToUtf8(FileName);
        if (path.empty()) {
            auto ctx = new ZfsFileContext{"", true, false, false};
            *PFileContext = ctx;
            self->populateFileInfo("", FileInfo);
            return STATUS_SUCCESS;
        }

        FSP_FSCTL_FILE_INFO fi{};
        self->populateFileInfo(path, &fi);
        if (fi.FileAttributes == 0) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        bool isDir = (fi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        bool isSnap = (path.rfind(".zfs", 0) == 0);
        auto ctx = new ZfsFileContext{path, isDir, false, isSnap};
        *PFileContext = ctx;
        *FileInfo = fi;
        return STATUS_SUCCESS;
    }

    static void CbClose(FSP_FILE_SYSTEM*, PVOID FileContext) {
        auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
        delete ctx;
    }

    static void CbCleanup(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR, ULONG Flags) {
        auto* self = FromFs(FileSystem);
        auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
        if (!self || !ctx || !self->dataset_) return;

        if (Flags & 1 /* CleanupDelete */) {
            if (ctx->path.rfind(".zfs", 0) != 0) {
                self->dataset_->removeFile(ctx->path);
            }
        }
    }

    static NTSTATUS CbGetFileInfo(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
        auto* self = FromFs(FileSystem);
        auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
        if (!self || !ctx) return STATUS_OBJECT_NAME_NOT_FOUND;

        self->populateFileInfo(ctx->path, FileInfo);
        return STATUS_SUCCESS;
    }

    static NTSTATUS CbRead(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
        auto* self = FromFs(FileSystem);
        auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
        if (!self || !ctx || ctx->isDirectory || !self->dataset_) return STATUS_FILE_IS_A_DIRECTORY;

        // Snapshot read
        if (ctx->path.rfind(".zfs/snapshot/", 0) == 0) {
            std::string sub = ctx->path.substr(14);
            size_t slash = sub.find('/');
            if (slash == std::string::npos) return STATUS_FILE_IS_A_DIRECTORY;

            std::string snapName = sub.substr(0, slash);
            std::string relPath = sub.substr(slash + 1);

            const auto& snaps = self->dataset_->getSnapshots();
            auto snapIt = snaps.find(snapName);
            if (snapIt == snaps.end()) return STATUS_OBJECT_NAME_NOT_FOUND;

            auto fit = snapIt->second.find(relPath);
            if (fit == snapIt->second.end()) return STATUS_OBJECT_NAME_NOT_FOUND;

            const auto& data = fit->second.data;
            if (Offset >= data.size()) {
                *PBytesTransferred = 0;
                return STATUS_END_OF_FILE;
            }

            size_t toCopy = std::min<size_t>(Length, data.size() - Offset);
            std::memcpy(Buffer, data.data() + Offset, toCopy);
            *PBytesTransferred = static_cast<ULONG>(toCopy);
            return STATUS_SUCCESS;
        }

        // Active dataset file read
        const auto& files = self->dataset_->getFiles();
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

    static NTSTATUS CbWrite(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, BOOLEAN, BOOLEAN, PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo) {
        auto* self = FromFs(FileSystem);
        auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
        if (!self || !ctx || ctx->isDirectory || !self->dataset_) return STATUS_FILE_IS_A_DIRECTORY;

        if (ctx->path.rfind(".zfs", 0) == 0) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }

        auto it = self->dataset_->getFiles().find(ctx->path);
        std::vector<uint8_t> currentData;
        if (it != self->dataset_->getFiles().end()) {
            currentData = it->second.data;
        }

        if (Offset + Length > currentData.size()) {
            currentData.resize(Offset + Length, 0);
        }
        std::memcpy(currentData.data() + Offset, Buffer, Length);

        self->dataset_->writeFile(ctx->path, currentData.data(), currentData.size(), 1);
        *PBytesTransferred = Length;
        self->populateFileInfo(ctx->path, FileInfo);
        return STATUS_SUCCESS;
    }

    static NTSTATUS CbSetFileSize(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, UINT64 NewSize, BOOLEAN, FSP_FSCTL_FILE_INFO* FileInfo) {
        auto* self = FromFs(FileSystem);
        auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
        if (!self || !ctx || ctx->isDirectory || !self->dataset_) return STATUS_FILE_IS_A_DIRECTORY;

        if (ctx->path.rfind(".zfs", 0) == 0) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }

        auto it = self->dataset_->getFiles().find(ctx->path);
        if (it != self->dataset_->getFiles().end()) {
            std::vector<uint8_t> data = it->second.data;
            data.resize(NewSize, 0);
            self->dataset_->writeFile(ctx->path, data.data(), data.size(), 1);
        }
        self->populateFileInfo(ctx->path, FileInfo);
        return STATUS_SUCCESS;
    }

    static NTSTATUS CbReadDirectory(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext, PWSTR, PWSTR Marker, PVOID Buffer, ULONG BufferLength, PULONG PBytesTransferred) {
        auto* self = FromFs(FileSystem);
        auto ctx = reinterpret_cast<ZfsFileContext*>(FileContext);
        if (!self || !ctx || !ctx->isDirectory || !self->dataset_) return STATUS_NOT_A_DIRECTORY;

        const std::string dirPrefix = ctx->path.empty() ? "" : (ctx->path + "/");
        std::string markerStr = Marker ? WideToUtf8(Marker) : "";

        auto& api = WinFspApi::Get();
        ULONG bytesTransferred = 0;

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

        if (markerStr.empty()) {
            FSP_FSCTL_FILE_INFO dotFi{};
            self->populateFileInfo(ctx->path, &dotFi);
            if (!addEntry(L".", dotFi)) {
                *PBytesTransferred = bytesTransferred;
                return STATUS_SUCCESS;
            }
        }
        if (markerStr.empty() || markerStr == ".") {
            FSP_FSCTL_FILE_INFO dotDotFi{};
            self->populateFileInfo("", &dotDotFi);
            if (!addEntry(L"..", dotDotFi)) {
                *PBytesTransferred = bytesTransferred;
                return STATUS_SUCCESS;
            }
        }

        std::map<std::string, bool> children;

        // Root directory exposes virtual .zfs
        if (ctx->path.empty()) {
            children[".zfs"] = true;
        } else if (ctx->path == ".zfs") {
            children["snapshot"] = true;
        } else if (ctx->path == ".zfs/snapshot") {
            for (const auto& [sname, _] : self->dataset_->getSnapshots()) {
                children[sname] = true;
            }
        } else if (ctx->path.rfind(".zfs/snapshot/", 0) == 0) {
            std::string sub = ctx->path.substr(14);
            size_t slash = sub.find('/');
            std::string snapName = (slash == std::string::npos) ? sub : sub.substr(0, slash);
            std::string snapRelPrefix = (slash == std::string::npos) ? "" : (sub.substr(slash + 1) + "/");

            const auto& snaps = self->dataset_->getSnapshots();
            auto snapIt = snaps.find(snapName);
            if (snapIt != snaps.end()) {
                for (const auto& [fpath, f] : snapIt->second) {
                    if (fpath.rfind(snapRelPrefix, 0) == 0) {
                        std::string rem = fpath.substr(snapRelPrefix.size());
                        size_t s = rem.find('/');
                        if (s == std::string::npos) {
                            children[rem] = f.isDirectory;
                        } else {
                            children[rem.substr(0, s)] = true;
                        }
                    }
                }
            }
        }

        // Active dataset children
        if (ctx->path.rfind(".zfs", 0) != 0) {
            for (const auto& [fpath, f] : self->dataset_->getFiles()) {
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
        }

        bool started = markerStr.empty() || markerStr == "." || markerStr == "..";
        for (const auto& [childName, isDir] : children) {
            if (!started) {
                if (childName == markerStr) {
                    started = true;
                }
                continue;
            }

            std::string fullChildPath = dirPrefix + childName;
            FSP_FSCTL_FILE_INFO cfi{};
            self->populateFileInfo(fullChildPath, &cfi);

            std::wstring wChildName(childName.begin(), childName.end());
            if (!addEntry(wChildName, cfi)) {
                break;
            }
        }

        *PBytesTransferred = bytesTransferred;
        return STATUS_SUCCESS;
    }

    std::shared_ptr<ZfsDataset> dataset_{nullptr};
    std::string mountPoint_;
    FSP_FILE_SYSTEM* fileSystem_{nullptr};
    bool mounted_{false};
};

} // namespace titanzfs

#endif // _WIN32
