#pragma once

#include "titanzfs/types.hpp"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#endif

namespace titanzfs {

// ============================================================================
// IPC Commands & Wire Protocol
// ============================================================================

enum IpcCommand : uint32_t {
    IpcCmdPing          = 0,
    IpcCmdCliPass       = 1, // Full CLI command pass (e.g., "zpool create tank ...")
    IpcCmdZpoolStatus   = 2,
    IpcCmdZpoolList     = 3,
    IpcCmdZpoolCreate   = 4,
    IpcCmdZpoolScrub    = 5,
    IpcCmdZpoolDestroy  = 6,
    IpcCmdZfsList       = 7,
    IpcCmdZfsCreate     = 8,
    IpcCmdZfsSnapshot   = 9,
    IpcCmdZfsRollback   = 10,
    IpcCmdZfsMount      = 11,
    IpcCmdZfsUnmount    = 12,
    IpcCmdServiceStatus = 13,
    IpcCmdSyncTxg       = 14
};

#pragma pack(push, 1)
struct IpcHeader {
    uint64_t magic{0x5A46534950433031ULL}; // "ZFSIPC01"
    uint32_t command{0};
    uint32_t payloadLength{0};
};

struct IpcResponseHeader {
    uint64_t magic{0x5A46534950433031ULL};
    uint32_t statusCode{0};
    uint32_t payloadLength{0};
};
#pragma pack(pop)

using IpcHandler = std::function<int(uint32_t cmd, const std::string& request, std::string& response)>;

// ============================================================================
// IPC Client
// ============================================================================

class IpcClient {
public:
    static bool isServerRunning(uint32_t timeoutMs = 150) {
#ifdef _WIN32
        const wchar_t* pipeName = L"\\\\.\\pipe\\titanzfs_ipc";
        if (WaitNamedPipeW(pipeName, timeoutMs)) {
            return true;
        }
        DWORD err = GetLastError();
        return (err == ERROR_SEM_TIMEOUT || err == ERROR_PIPE_BUSY || err == ERROR_ACCESS_DENIED);
#else
        (void)timeoutMs;
        const char* sockPath = "/tmp/titanzfs_ipc.sock";
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return false;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);
        bool ok = (connect(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
        close(s);
        return ok;
#endif
    }

    static int send(uint32_t cmd, const std::string& request, std::string& response, uint32_t timeoutMs = 5000) {
#ifdef _WIN32
        const wchar_t* pipeName = L"\\\\.\\pipe\\titanzfs_ipc";
        if (!WaitNamedPipeW(pipeName, timeoutMs)) {
            if (GetLastError() != ERROR_PIPE_BUSY) return -1;
            if (!WaitNamedPipeW(pipeName, timeoutMs)) return -1;
        }

        HANDLE hPipe = CreateFileW(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            hPipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hPipe == INVALID_HANDLE_VALUE) return -1;
        }

        IpcHeader reqHdr{
            .magic = 0x5A46534950433031ULL,
            .command = cmd,
            .payloadLength = static_cast<uint32_t>(request.size())
        };

        DWORD written = 0;
        if (!WriteFile(hPipe, &reqHdr, sizeof(reqHdr), &written, nullptr)) {
            CloseHandle(hPipe);
            return -1;
        }

        if (!request.empty()) {
            if (!WriteFile(hPipe, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)) {
                CloseHandle(hPipe);
                return -1;
            }
        }

        IpcResponseHeader respHdr{};
        DWORD bytesRead = 0;
        if (!ReadFile(hPipe, &respHdr, sizeof(respHdr), &bytesRead, nullptr) || bytesRead != sizeof(respHdr)) {
            CloseHandle(hPipe);
            return -1;
        }

        if (respHdr.payloadLength > 0) {
            response.resize(respHdr.payloadLength);
            DWORD totalRead = 0;
            while (totalRead < respHdr.payloadLength) {
                DWORD chunk = 0;
                if (!ReadFile(hPipe, response.data() + totalRead, respHdr.payloadLength - totalRead, &chunk, nullptr) || chunk == 0) {
                    break;
                }
                totalRead += chunk;
            }
        } else {
            response.clear();
        }

        CloseHandle(hPipe);
        return static_cast<int>(respHdr.statusCode);
#else
        (void)timeoutMs;
        const char* sockPath = "/tmp/titanzfs_ipc.sock";
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return -1;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);

        if (connect(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(s);
            return -1;
        }

        IpcHeader reqHdr{
            .magic = 0x5A46534950433031ULL,
            .command = cmd,
            .payloadLength = static_cast<uint32_t>(request.size())
        };

        if (::write(s, &reqHdr, sizeof(reqHdr)) != sizeof(reqHdr)) {
            close(s);
            return -1;
        }

        if (!request.empty()) {
            if (::write(s, request.data(), request.size()) < 0) {
                close(s);
                return -1;
            }
        }

        IpcResponseHeader respHdr{};
        if (::read(s, &respHdr, sizeof(respHdr)) != sizeof(respHdr)) {
            close(s);
            return -1;
        }

        if (respHdr.payloadLength > 0) {
            response.resize(respHdr.payloadLength);
            size_t total = 0;
            while (total < respHdr.payloadLength) {
                ssize_t n = ::read(s, response.data() + total, respHdr.payloadLength - total);
                if (n <= 0) break;
                total += n;
            }
        } else {
            response.clear();
        }

        close(s);
        return static_cast<int>(respHdr.statusCode);
#endif
    }
};

// ============================================================================
// IPC Server
// ============================================================================

class IpcServer {
public:
    IpcServer() = default;
    ~IpcServer() { stop(); }

    bool start(IpcHandler handler) {
        if (running_) return true;
        handler_ = std::move(handler);
        running_ = true;
        serverThread_ = std::thread(&IpcServer::listenLoop, this);
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        // Wake up listener
        std::string dummyResp;
        IpcClient::send(IpcCmdPing, "", dummyResp, 100);

        if (serverThread_.joinable()) {
            serverThread_.join();
        }
#ifndef _WIN32
        unlink("/tmp/titanzfs_ipc.sock");
#endif
    }

    [[nodiscard]] bool isRunning() const noexcept { return running_; }

private:
    void listenLoop() {
#ifdef _WIN32
        const wchar_t* pipeName = L"\\\\.\\pipe\\titanzfs_ipc";

        HANDLE hPipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            1000,
            nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            return;
        }

        while (running_) {
            BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (!running_) break;

            IpcHeader reqHdr{};
            DWORD bytesRead = 0;
            if (ReadFile(hPipe, &reqHdr, sizeof(reqHdr), &bytesRead, nullptr) && bytesRead == sizeof(reqHdr)) {
                if (reqHdr.magic == 0x5A46534950433031ULL) {
                    std::string requestStr;
                    if (reqHdr.payloadLength > 0) {
                        requestStr.resize(reqHdr.payloadLength);
                        DWORD totalRead = 0;
                        while (totalRead < reqHdr.payloadLength) {
                            DWORD chunk = 0;
                            if (!ReadFile(hPipe, requestStr.data() + totalRead, reqHdr.payloadLength - totalRead, &chunk, nullptr) || chunk == 0) {
                                break;
                            }
                            totalRead += chunk;
                        }
                    }

                    std::string responseStr;
                    int statusCode = 0;
                    if (handler_) {
                        statusCode = handler_(reqHdr.command, requestStr, responseStr);
                    }

                    IpcResponseHeader respHdr{
                        .magic = 0x5A46534950433031ULL,
                        .statusCode = static_cast<uint32_t>(statusCode),
                        .payloadLength = static_cast<uint32_t>(responseStr.size())
                    };

                    DWORD written = 0;
                    WriteFile(hPipe, &respHdr, sizeof(respHdr), &written, nullptr);
                    if (!responseStr.empty()) {
                        WriteFile(hPipe, responseStr.data(), static_cast<DWORD>(responseStr.size()), &written, nullptr);
                    }
                    FlushFileBuffers(hPipe);
                }
            }

            DisconnectNamedPipe(hPipe);
        }

        CloseHandle(hPipe);
#else
        const char* sockPath = "/tmp/titanzfs_ipc.sock";
        unlink(sockPath);

        int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (serverFd < 0) return;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);

        if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(serverFd);
            return;
        }

        if (listen(serverFd, 10) < 0) {
            close(serverFd);
            return;
        }

        while (running_) {
            struct pollfd pfd{ .fd = serverFd, .events = POLLIN, .revents = 0 };
            int pollRet = poll(&pfd, 1, 500);
            if (pollRet <= 0 || !(pfd.revents & POLLIN)) continue;

            int clientFd = accept(serverFd, nullptr, nullptr);
            if (clientFd < 0) continue;

            IpcHeader reqHdr{};
            if (::read(clientFd, &reqHdr, sizeof(reqHdr)) == sizeof(reqHdr)) {
                if (reqHdr.magic == 0x5A46534950433031ULL) {
                    std::string reqStr;
                    if (reqHdr.payloadLength > 0) {
                        reqStr.resize(reqHdr.payloadLength);
                        size_t total = 0;
                        while (total < reqHdr.payloadLength) {
                            ssize_t n = ::read(clientFd, reqStr.data() + total, reqHdr.payloadLength - total);
                            if (n <= 0) break;
                            total += n;
                        }
                    }

                    std::string respStr;
                    int statusCode = 0;
                    if (handler_) {
                        statusCode = handler_(reqHdr.command, reqStr, respStr);
                    }

                    IpcResponseHeader respHdr{
                        .magic = 0x5A46534950433031ULL,
                        .statusCode = static_cast<uint32_t>(statusCode),
                        .payloadLength = static_cast<uint32_t>(respStr.size())
                    };

                    if (::write(clientFd, &respHdr, sizeof(respHdr)) > 0 && !respStr.empty()) {
                        (void)::write(clientFd, respStr.data(), respStr.size());
                    }
                }
            }
            close(clientFd);
        }
        close(serverFd);
        unlink(sockPath);
#endif
    }

    std::atomic<bool> running_{false};
    std::thread serverThread_;
    IpcHandler handler_;
};

} // namespace titanzfs
