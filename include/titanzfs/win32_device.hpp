#pragma once

#include "titanzfs/types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace titanzfs {

// ============================================================================
// Production Win32 Raw Physical Drive & Raw Image Block Device
// ============================================================================

class Win32BlockDevice : public IBlockDevice {
public:
    Win32BlockDevice(std::string name, std::string targetPath, uint64_t blockSize = 4096, uint64_t fallbackSize = 64 * 1024 * 1024)
        : name_(std::move(name)), targetPath_(std::move(targetPath)), blockSize_(blockSize) {
        openDevice(fallbackSize);
    }

    ~Win32BlockDevice() override {
        closeDevice();
    }

    // Move-only semantics for safe handle ownership
    Win32BlockDevice(const Win32BlockDevice&) = delete;
    Win32BlockDevice& operator=(const Win32BlockDevice&) = delete;

    [[nodiscard]] uint64_t getBlockSize() const noexcept override {
        return blockSize_;
    }

    [[nodiscard]] uint64_t getTotalBlocks() const noexcept override {
        return totalBlocks_;
    }

    [[nodiscard]] bool isOnline() const noexcept override {
        return isOnline_;
    }

    void setOnline(bool online) noexcept override {
        isOnline_ = online;
    }

    [[nodiscard]] const std::string& getDeviceName() const noexcept override {
        return name_;
    }

    [[nodiscard]] bool isPhysicalDrive() const noexcept {
        return isPhysicalDrive_;
    }

    // ------------------------------------------------------------------------
    // Read Blocks (with automatic sector alignment bounce buffer)
    // ------------------------------------------------------------------------
    Status readBlocks(uint64_t lba, uint64_t count, void* buffer) override {
        if (!isOnline_) return Status::NoSuchDevice;
        if (lba + count > totalBlocks_) return Status::InvalidParameter;

#ifdef _WIN32
        if (handle_ == INVALID_HANDLE_VALUE) return Status::NoSuchDevice;

        const uint64_t offset = lba * blockSize_;
        const uint64_t bytesToRead = count * blockSize_;

        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(offset);

        // Check if buffer pointer is aligned to 4KB (required for FILE_FLAG_NO_BUFFERING)
        const bool isAligned = (reinterpret_cast<uintptr_t>(buffer) % blockSize_ == 0);

        if (isAligned && !useBufferedFallback_) {
            OVERLAPPED ov{};
            ov.Offset = li.LowPart;
            ov.OffsetHigh = li.HighPart;

            DWORD bytesRead = 0;
            if (!ReadFile(handle_, buffer, static_cast<DWORD>(bytesToRead), &bytesRead, &ov)) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    return Status::DeviceError;
                }
                if (!GetOverlappedResult(handle_, &ov, &bytesRead, TRUE)) {
                    return Status::DeviceError;
                }
            }
            if (bytesRead != bytesToRead) return Status::DeviceError;
        } else {
            // Allocate 4KB-aligned bounce buffer
            void* bounce = _aligned_malloc(bytesToRead, static_cast<size_t>(blockSize_));
            if (!bounce) return Status::DiskFull;

            OVERLAPPED ov{};
            ov.Offset = li.LowPart;
            ov.OffsetHigh = li.HighPart;

            DWORD bytesRead = 0;
            BOOL ok = ReadFile(handle_, bounce, static_cast<DWORD>(bytesToRead), &bytesRead, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                ok = GetOverlappedResult(handle_, &ov, &bytesRead, TRUE);
            }

            if (ok && bytesRead == bytesToRead) {
                std::memcpy(buffer, bounce, bytesToRead);
                _aligned_free(bounce);
            } else {
                _aligned_free(bounce);
                return Status::DeviceError;
            }
        }
        return Status::Success;
#else
        if (fd_ < 0) return Status::NoSuchDevice;
        const off_t offset = static_cast<off_t>(lba * blockSize_);
        const size_t bytesToRead = static_cast<size_t>(count * blockSize_);
        ssize_t res = pread(fd_, buffer, bytesToRead, offset);
        return (res == static_cast<ssize_t>(bytesToRead)) ? Status::Success : Status::DeviceError;
#endif
    }

    // ------------------------------------------------------------------------
    // Write Blocks (with direct-I/O flush)
    // ------------------------------------------------------------------------
    Status writeBlocks(uint64_t lba, uint64_t count, const void* buffer) override {
        if (!isOnline_) return Status::NoSuchDevice;
        if (lba + count > totalBlocks_) return Status::InvalidParameter;

#ifdef _WIN32
        if (handle_ == INVALID_HANDLE_VALUE) return Status::NoSuchDevice;

        const uint64_t offset = lba * blockSize_;
        const uint64_t bytesToWrite = count * blockSize_;

        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(offset);

        const bool isAligned = (reinterpret_cast<uintptr_t>(buffer) % blockSize_ == 0);

        if (isAligned && !useBufferedFallback_) {
            OVERLAPPED ov{};
            ov.Offset = li.LowPart;
            ov.OffsetHigh = li.HighPart;

            DWORD bytesWritten = 0;
            if (!WriteFile(handle_, buffer, static_cast<DWORD>(bytesToWrite), &bytesWritten, &ov)) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    return Status::DeviceError;
                }
                if (!GetOverlappedResult(handle_, &ov, &bytesWritten, TRUE)) {
                    return Status::DeviceError;
                }
            }
            if (bytesWritten != bytesToWrite) return Status::DeviceError;
        } else {
            void* bounce = _aligned_malloc(bytesToWrite, static_cast<size_t>(blockSize_));
            if (!bounce) return Status::DiskFull;

            std::memcpy(bounce, buffer, bytesToWrite);

            OVERLAPPED ov{};
            ov.Offset = li.LowPart;
            ov.OffsetHigh = li.HighPart;

            DWORD bytesWritten = 0;
            BOOL ok = WriteFile(handle_, bounce, static_cast<DWORD>(bytesToWrite), &bytesWritten, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                ok = GetOverlappedResult(handle_, &ov, &bytesWritten, TRUE);
            }

            _aligned_free(bounce);
            if (!ok || bytesWritten != bytesToWrite) {
                return Status::DeviceError;
            }
        }
        return Status::Success;
#else
        if (fd_ < 0) return Status::NoSuchDevice;
        const off_t offset = static_cast<off_t>(lba * blockSize_);
        const size_t bytesToWrite = static_cast<size_t>(count * blockSize_);
        ssize_t res = pwrite(fd_, buffer, bytesToWrite, offset);
        return (res == static_cast<ssize_t>(bytesToWrite)) ? Status::Success : Status::DeviceError;
#endif
    }

private:
    void openDevice(uint64_t fallbackSize) {
#ifdef _WIN32
        std::wstring wpath;
        if (targetPath_.find(R"(\\.\)") == 0 || targetPath_.find(R"(\??\)") == 0) {
            wpath = std::wstring(targetPath_.begin(), targetPath_.end());
            isPhysicalDrive_ = true;
        } else if (targetPath_.find("PhysicalDrive") == 0) {
            std::string full = R"(\\.\)" + targetPath_;
            wpath = std::wstring(full.begin(), full.end());
            isPhysicalDrive_ = true;
        } else {
            wpath = std::wstring(targetPath_.begin(), targetPath_.end());
            isPhysicalDrive_ = false;
        }

        DWORD flags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
        DWORD creation = isPhysicalDrive_ ? OPEN_EXISTING : OPEN_ALWAYS;

        handle_ = CreateFileW(
            wpath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            creation,
            flags,
            nullptr
        );

        if (handle_ == INVALID_HANDLE_VALUE) {
            // Fall back without FILE_FLAG_NO_BUFFERING if requested by file system or permissions
            flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH;
            handle_ = CreateFileW(
                wpath.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                creation,
                flags,
                nullptr
            );
            useBufferedFallback_ = true;
        }

        if (handle_ != INVALID_HANDLE_VALUE) {
            isOnline_ = true;
            uint64_t totalBytes = 0;

            if (isPhysicalDrive_) {
                DISK_GEOMETRY_EX geo{};
                DWORD bytesReturned = 0;
                if (DeviceIoControl(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &bytesReturned, nullptr)) {
                    totalBytes = static_cast<uint64_t>(geo.DiskSize.QuadPart);
                    if (geo.Geometry.BytesPerSector > 0) {
                        blockSize_ = geo.Geometry.BytesPerSector;
                    }
                } else {
                    GET_LENGTH_INFORMATION lengthInfo{};
                    if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &lengthInfo, sizeof(lengthInfo), &bytesReturned, nullptr)) {
                        totalBytes = static_cast<uint64_t>(lengthInfo.Length.QuadPart);
                    }
                }
            } else {
                LARGE_INTEGER fileSize{};
                if (GetFileSizeEx(handle_, &fileSize) && fileSize.QuadPart > 0) {
                    totalBytes = static_cast<uint64_t>(fileSize.QuadPart);
                } else {
                    // Set initial size for sparse/raw virtual test image
                    LARGE_INTEGER newSize;
                    newSize.QuadPart = static_cast<LONGLONG>(fallbackSize);
                    SetFilePointerEx(handle_, newSize, nullptr, FILE_BEGIN);
                    SetEndOfFile(handle_);
                    totalBytes = fallbackSize;
                }
            }

            if (totalBytes == 0) totalBytes = fallbackSize;
            totalBlocks_ = totalBytes / blockSize_;
        } else {
            isOnline_ = false;
            totalBlocks_ = 0;
        }
#else
        fd_ = open(targetPath_.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ >= 0) {
            isOnline_ = true;
            struct stat st{};
            if (fstat(fd_, &st) == 0 && st.st_size > 0) {
                totalBlocks_ = static_cast<uint64_t>(st.st_size) / blockSize_;
            } else {
                if (ftruncate(fd_, static_cast<off_t>(fallbackSize)) == 0) {
                    totalBlocks_ = fallbackSize / blockSize_;
                } else {
                    totalBlocks_ = 1024;
                }
            }
        } else {
            isOnline_ = false;
            totalBlocks_ = 0;
        }
#endif
    }

    void closeDevice() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
#endif
        isOnline_ = false;
    }

    std::string name_;
    std::string targetPath_;
    uint64_t blockSize_{4096};
    uint64_t totalBlocks_{0};
    bool isOnline_{false};
    bool isPhysicalDrive_{false};
    bool useBufferedFallback_{false};

#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int fd_{-1};
#endif
};

// Convenience factory for raw block devices
inline std::shared_ptr<IBlockDevice> CreateWin32BlockDevice(
    std::string name,
    std::string path,
    uint64_t blockSize = 4096,
    uint64_t fallbackSize = 64 * 1024 * 1024
) {
    return std::make_shared<Win32BlockDevice>(std::move(name), std::move(path), blockSize, fallbackSize);
}

} // namespace titanzfs
