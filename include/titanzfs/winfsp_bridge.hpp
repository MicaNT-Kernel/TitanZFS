#pragma once

#include "titanzfs/types.hpp"
#include <cstdint>
#include <string>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace titanzfs {

// ============================================================================
// Clean-Room WinFsp Types & Definitions
// ============================================================================

typedef void* PSECURITY_DESCRIPTOR;
typedef LONG NTSTATUS;

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED           ((NTSTATUS)0xC0000002L)
#define STATUS_ACCESS_DENIED             ((NTSTATUS)0xC0000022L)
#define STATUS_OBJECT_NAME_NOT_FOUND     ((NTSTATUS)0xC0000034L)
#define STATUS_OBJECT_NAME_COLLISION     ((NTSTATUS)0xC0000035L)
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
        // Try standard DLL search path
        hModule_ = LoadLibraryW(L"winfsp-x64.dll");
        if (!hModule_) {
            // Check default WinFsp installation directory
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

} // namespace titanzfs

#endif // _WIN32
