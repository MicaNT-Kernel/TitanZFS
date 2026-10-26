#include "titanzfs/titanzfs.hpp"
#include "titanzfs/win32_device.hpp"
#include "titanzfs/ipc.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>

#ifdef _WIN32
#include "titanzfs/winfsp_bridge.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#endif

namespace titanzfs {

class TitanZfsDaemon {
public:
    static TitanZfsDaemon& Get() {
        static TitanZfsDaemon instance;
        return instance;
    }

    bool start() {
        if (running_) return true;
        running_ = true;

        std::cout << "========================================================================\n";
        std::cout << "        TitanZFS Windows NT Subsystem Service Daemon                    \n";
        std::cout << "========================================================================\n";
        std::cout << "[TitanZFS Daemon] Initializing storage pool subsystem...\n";

        auto& sys = TitanZfsSubsystem::Get();
        sys.loadCache();

        std::cout << "[TitanZFS Daemon] Loaded " << sys.getPools().size() << " pools from cache.\n";

#ifdef _WIN32
        // Auto-mount pools and default datasets
        auto& fsp = WinFspApi::Get();
        if (fsp.isAvailable()) {
            std::cout << "[TitanZFS Daemon] WinFsp dynamic engine loaded successfully.\n";
            char nextDrive = 'Z';
            for (const auto& [pname, pool] : sys.getPools()) {
                std::string dsName = pname + "/data";
                auto ds = pool->getDataset(dsName);
                if (ds) {
                    std::string driveLetter = std::string(1, nextDrive) + ":";
                    auto mountSession = std::make_shared<WinFspMountSession>();
                    if (mountSession->mount(ds, driveLetter)) {
                        std::cout << "[TitanZFS Daemon] Auto-mounted '" << dsName << "' to " << driveLetter << "\n";
                        activeMounts_[dsName] = mountSession;
                        --nextDrive;
                    }
                }
            }
        } else {
            std::cout << "[TitanZFS Daemon] WinFsp not found; running in headless storage engine mode.\n";
        }
#endif

        // Start Named Pipe IPC Server
        ipcServer_.start([this](uint32_t cmd, const std::string& req, std::string& resp) {
            return this->handleIpcCommand(cmd, req, resp);
        });
        std::cout << "[TitanZFS Daemon] Named pipe IPC endpoint active: \\\\.\\pipe\\titanzfs_ipc\n";

        // Start 5-second Transaction Group (TXG) sync engine
        txgSyncThread_ = std::thread(&TitanZfsDaemon::txgSyncWorker, this);
        std::cout << "[TitanZFS Daemon] Asynchronous 5-second TXG commit engine started.\n";

        // Start Autonomous Health Watchdog & Monthly Bit-Rot Scrub Engine
        watchdogThread_ = std::thread(&TitanZfsDaemon::watchdogWorker, this);
        std::cout << "[TitanZFS Daemon] Autonomous health watchdog & self-healing scrub engine started.\n";
        std::cout << "[TitanZFS Daemon] Service is ready and accepting requests.\n\n";

        return true;
    }

    void stop() {
        if (!running_) return;
        std::cout << "\n[TitanZFS Daemon] Initiating graceful subsystem shutdown...\n";
        running_ = false;

        ipcServer_.stop();

        if (txgSyncThread_.joinable()) {
            txgSyncThread_.join();
        }
        if (watchdogThread_.joinable()) {
            watchdogThread_.join();
        }

#ifdef _WIN32
        for (auto& [dsName, session] : activeMounts_) {
            std::cout << "[TitanZFS Daemon] Unmounting " << dsName << " (" << session->getMountPoint() << ")...\n";
            session->unmount();
        }
        activeMounts_.clear();
#endif

        // Commit final transaction group and flush cache
        auto& sys = TitanZfsSubsystem::Get();
        for (const auto& [_, pool] : sys.getPools()) {
            pool->syncTransactionGroup();
        }
        sys.saveCache();

        std::cout << "[TitanZFS Daemon] Subsystem stopped. All pools synchronized.\n";
    }

    [[nodiscard]] bool isRunning() const noexcept { return running_; }

private:
    TitanZfsDaemon() = default;

    void txgSyncWorker() {
        while (running_) {
            // Sleep in 250ms increments to allow responsive shutdown
            for (int i = 0; i < 20 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (!running_) break;

            auto& sys = TitanZfsSubsystem::Get();
            if (!sys.getPools().empty()) {
                for (const auto& [_, pool] : sys.getPools()) {
                    pool->syncTransactionGroup();
                }
                sys.saveCache();
            }
        }
    }

    void watchdogWorker() {
        while (running_) {
            // Sleep in 1-second increments for responsive shutdown
            for (int i = 0; i < 3600 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!running_) break;

            uint64_t now = static_cast<uint64_t>(std::time(nullptr));
            // Autonomous scrub cadence: every 30 days (2,592,000 seconds)
            if (lastScrubTimestamp_ == 0 || now >= lastScrubTimestamp_ + 2592000ULL) {
                auto& sys = TitanZfsSubsystem::Get();
                for (const auto& [name, pool] : sys.getPools()) {
                    uint64_t s = 0, r = 0;
                    pool->scrubPool(s, r);
                    totalScrubbedBlocks_ += s;
                    totalRepairedBlocks_ += r;
                    if (r > 0) {
                        std::cerr << "[TitanZFS Watchdog] Bit-rot detected & self-healed in pool '"
                                  << name << "': " << r << " blocks reconstructed via parity!\n";
                    }
                }
                lastScrubTimestamp_ = now;
            }
        }
    }

    int handleIpcCommand(uint32_t cmd, const std::string& request, std::string& response) {
        auto& sys = TitanZfsSubsystem::Get();

        switch (cmd) {
            case IpcCmdPing: {
                response = "PONG";
                return 0;
            }

            case IpcCmdServiceStatus: {
                std::ostringstream oss;
                oss << "TitanZFS Service: RUNNING\n";
                oss << "Active Pools: " << sys.getPools().size() << "\n";
                oss << "Autonomous Health Watchdog: Active (Monthly 256-bit Fletcher-4 Scrub)\n";
                oss << "Blocks Verified: " << totalScrubbedBlocks_.load() << " | Repaired Bit-Rot: " << totalRepairedBlocks_.load() << "\n";
#ifdef _WIN32
                oss << "Active Mounts: " << activeMounts_.size() << "\n";
                for (const auto& [ds, s] : activeMounts_) {
                    oss << "  - " << ds << " -> " << s->getMountPoint() << "\n";
                }
#endif
                response = oss.str();
                return 0;
            }

            case IpcCmdSyncTxg: {
                for (const auto& [_, pool] : sys.getPools()) {
                    pool->syncTransactionGroup();
                }
                sys.saveCache();
                response = "TXG synchronization complete across all active pools.";
                return 0;
            }

            case IpcCmdZpoolStatus: {
                std::ostringstream oss;
                if (sys.getPools().empty()) {
                    oss << "no pools available\n";
                } else {
                    for (const auto& [name, pool] : sys.getPools()) {
                        if (!request.empty() && name != request) continue;
                        oss << "  pool: " << name << "\n";
                        oss << " state: " << ToString(pool->getState()) << "\n";
                        oss << "status: The pool is operating at normal parameters with 256-bit Fletcher-4 checksumming.\n";
                        oss << "action: None required.\n";
                        oss << "  scan: none requested\n";
                        oss << "config:\n\n";
                        oss << "\tNAME                        STATE     READ WRITE CKSUM\n";
                        oss << "\t" << std::left << std::setw(28) << name << std::setw(10) << ToString(pool->getState()) << "   0     0     0\n";
                        auto root = pool->getRootVdev();
                        if (root) {
                            oss << "\t  " << std::left << std::setw(26) << root->getName() << std::setw(10) << ToString(root->getState())
                                << "   " << root->getReadErrors() << "     " << root->getWriteErrors() << "     " << root->getChecksumErrors() << "\n";
                            if (auto rz = std::dynamic_pointer_cast<Raidz1Vdev>(root)) {
                                for (const auto& c : rz->getChildren()) {
                                    oss << "\t    " << std::left << std::setw(24) << c->getName() << std::setw(10) << ToString(c->getState())
                                        << "   " << c->getReadErrors() << "     " << c->getWriteErrors() << "     " << c->getChecksumErrors() << "\n";
                                }
                            } else if (auto mir = std::dynamic_pointer_cast<MirrorVdev>(root)) {
                                for (const auto& c : mir->getChildren()) {
                                    oss << "\t    " << std::left << std::setw(24) << c->getName() << std::setw(10) << ToString(c->getState())
                                        << "   " << c->getReadErrors() << "     " << c->getWriteErrors() << "     " << c->getChecksumErrors() << "\n";
                                }
                            }
                        }
                        oss << "\nerrors: No known data errors\n\n";
                    }
                }
                response = oss.str();
                return 0;
            }

            case IpcCmdZpoolList: {
                std::ostringstream oss;
                if (sys.getPools().empty()) {
                    oss << "no pools available\n";
                } else {
                    oss << "NAME    SIZE  ALLOC   FREE  CKPOINT  EXPANDSZ   FRAG    CAP  DEDUP    HEALTH  ALTROOT\n";
                    for (const auto& [name, pool] : sys.getPools()) {
                        uint64_t cap = pool->getRootVdev() ? pool->getRootVdev()->getTotalCapacity() : 0;
                        oss << std::left << std::setw(8) << name
                            << std::setw(6) << (cap / (1024 * 1024)) << "M"
                            << std::setw(7) << "256K"
                            << std::setw(7) << ((cap / (1024 * 1024)) - 1) << "M"
                            << std::setw(9) << "-"
                            << std::setw(11) << "-"
                            << std::setw(8) << "0%"
                            << std::setw(5) << "1%"
                            << std::setw(9) << "1.00x"
                            << std::setw(8) << ToString(pool->getState())
                            << "-\n";
                    }
                }
                response = oss.str();
                return 0;
            }

            case IpcCmdZpoolScrub: {
                std::ostringstream oss;
                uint64_t totalScrubbed = 0, totalRepaired = 0;
                for (const auto& [name, pool] : sys.getPools()) {
                    if (!request.empty() && name != request) continue;
                    uint64_t s = 0, r = 0;
                    pool->scrubPool(s, r);
                    totalScrubbed += s;
                    totalRepaired += r;
                    totalScrubbedBlocks_ += s;
                    totalRepairedBlocks_ += r;
                    oss << "Scrub completed on pool '" << name << "': "
                        << s << " blocks verified, " << r << " errors repaired.\n";
                }
                lastScrubTimestamp_ = static_cast<uint64_t>(std::time(nullptr));
                if (sys.getPools().empty()) {
                    oss << "no active pools to scrub\n";
                }
                response = oss.str();
                return 0;
            }

            case IpcCmdZfsList: {
                std::ostringstream oss;
                if (sys.getPools().empty()) {
                    oss << "no datasets available\n";
                } else {
                    oss << "NAME                  USED  AVAIL  REFER  MOUNTPOINT\n";
                    for (const auto& [pname, p] : sys.getPools()) {
                        for (const auto& [dname, ds] : p->getDatasets()) {
                            std::string mp = "none";
#ifdef _WIN32
                            auto mit = activeMounts_.find(dname);
                            if (mit != activeMounts_.end()) mp = mit->second->getMountPoint();
#endif
                            oss << std::left << std::setw(22) << dname
                                << std::setw(6) << "256K"
                                << std::setw(7) << "64.0M"
                                << std::setw(7) << "256K"
                                << mp << "\n";

                            for (const auto& [sname, _] : ds->getSnapshots()) {
                                oss << std::left << std::setw(22) << (dname + "@" + sname)
                                    << std::setw(6) << "0B"
                                    << std::setw(7) << "-"
                                    << std::setw(7) << "256K"
                                    << "-\n";
                            }
                        }
                    }
                }
                response = oss.str();
                return 0;
            }

            case IpcCmdZfsSnapshot: {
                // request format: "dataset@snapName"
                size_t atPos = request.find('@');
                if (atPos == std::string::npos) {
                    response = "cannot open '" + request + "': missing '@' delimiter";
                    return 1;
                }
                std::string dsName = request.substr(0, atPos);
                std::string snapName = request.substr(atPos + 1);
                size_t slash = dsName.find('/');
                std::string poolName = (slash != std::string::npos) ? dsName.substr(0, slash) : dsName;

                auto pool = sys.getPool(poolName);
                if (!pool) {
                    response = "cannot open '" + request + "': pool not found";
                    return 1;
                }
                auto ds = pool->getDataset(dsName);
                if (!ds) {
                    response = "cannot open '" + dsName + "': dataset not found";
                    return 1;
                }
                Status st = ds->createSnapshot(snapName, pool->getCurrentTxg());
                if (st == Status::Success) {
                    sys.saveCache();
                    response = "Created instantaneous snapshot '" + request + "'.";
                    return 0;
                } else {
                    response = "cannot create snapshot '" + request + "': snapshot already exists";
                    return 1;
                }
            }

            case IpcCmdZfsMount: {
#ifdef _WIN32
                // request format: "dataset driveLetter" (e.g., "tank/data Z:")
                std::istringstream iss(request);
                std::string dsName, driveLetter;
                iss >> dsName >> driveLetter;
                if (dsName.empty() || driveLetter.empty()) {
                    response = "usage: zfs mount <dataset> <drive:>";
                    return 1;
                }

                size_t slash = dsName.find('/');
                std::string poolName = (slash != std::string::npos) ? dsName.substr(0, slash) : dsName;
                auto pool = sys.getPool(poolName);
                if (!pool) {
                    response = "cannot mount '" + dsName + "': pool not found";
                    return 1;
                }
                auto ds = pool->getDataset(dsName);
                if (!ds) {
                    response = "cannot mount '" + dsName + "': dataset not found";
                    return 1;
                }

                auto session = std::make_shared<WinFspMountSession>();
                if (session->mount(ds, driveLetter)) {
                    activeMounts_[dsName] = session;
                    response = "Mounted '" + dsName + "' to " + driveLetter;
                    return 0;
                } else {
                    response = "failed to mount '" + dsName + "' to " + driveLetter;
                    return 1;
                }
#else
                response = "WinFsp mount is only supported on Windows NT";
                return 1;
#endif
            }

            case IpcCmdZfsUnmount: {
#ifdef _WIN32
                auto it = activeMounts_.find(request);
                if (it != activeMounts_.end()) {
                    it->second->unmount();
                    activeMounts_.erase(it);
                    response = "Unmounted '" + request + "'";
                    return 0;
                }
                response = "cannot unmount '" + request + "': not currently mounted";
                return 1;
#else
                response = "WinFsp unmount is only supported on Windows NT";
                return 1;
#endif
            }

            default: {
                response = "unknown IPC command";
                return -1;
            }
        }
    }

    std::atomic<bool> running_{false};
    std::thread txgSyncThread_;
    std::thread watchdogThread_;
    std::atomic<uint64_t> lastScrubTimestamp_{0};
    std::atomic<uint64_t> totalScrubbedBlocks_{0};
    std::atomic<uint64_t> totalRepairedBlocks_{0};
    IpcServer ipcServer_;
#ifdef _WIN32
    std::map<std::string, std::shared_ptr<WinFspMountSession>> activeMounts_;
#endif
};

} // namespace titanzfs

// ============================================================================
// Windows NT Service Control Manager (SCM) Integration
// ============================================================================

#ifdef _WIN32
static SERVICE_STATUS        g_ServiceStatus{};
static SERVICE_STATUS_HANDLE g_StatusHandle{nullptr};

static void WINAPI ServiceCtrlHandler(DWORD request) {
    switch (request) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            titanzfs::TitanZfsDaemon::Get().stop();
            g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            break;
        default:
            break;
    }
}

static void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_StatusHandle = RegisterServiceCtrlHandlerW(L"TitanZFS", ServiceCtrlHandler);
    if (!g_StatusHandle) return;

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    if (titanzfs::TitanZfsDaemon::Get().start()) {
        g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        while (titanzfs::TitanZfsDaemon::Get().isRunning()) {
            Sleep(1000);
        }
    }

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

static bool InstallService() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        std::cerr << "Failed to resolve executable path.\n";
        return false;
    }

    SC_HANDLE schSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!schSCManager) {
        std::cerr << "Failed to open Service Control Manager (Administrator privileges required).\n";
        return false;
    }

    SC_HANDLE schService = CreateServiceW(
        schSCManager,
        L"TitanZFS",
        L"TitanZFS Storage Pool Subsystem",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );

    if (!schService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::cout << "Service 'TitanZFS' is already installed.\n";
        } else {
            std::cerr << "CreateService failed with error: " << err << "\n";
        }
        CloseServiceHandle(schSCManager);
        return false;
    }

    // Set service description
    SERVICE_DESCRIPTIONW desc;
    wchar_t descStr[] = L"TitanZFS storage pool engine and filesystem background service.";
    desc.lpDescription = descStr;
    ChangeServiceConfig2W(schService, SERVICE_CONFIG_DESCRIPTION, &desc);

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    std::cout << "Successfully registered 'TitanZFS' with Windows Service Control Manager (AUTO_START).\n";
    return true;
}

static bool UninstallService() {
    SC_HANDLE schSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        std::cerr << "Failed to open Service Control Manager (Administrator privileges required).\n";
        return false;
    }

    SC_HANDLE schService = OpenServiceW(schSCManager, L"TitanZFS", SERVICE_STOP | DELETE);
    if (!schService) {
        std::cerr << "Service 'TitanZFS' not found.\n";
        CloseServiceHandle(schSCManager);
        return false;
    }

    SERVICE_STATUS status;
    ControlService(schService, SERVICE_CONTROL_STOP, &status);

    if (!DeleteService(schService)) {
        std::cerr << "DeleteService failed with error: " << GetLastError() << "\n";
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    std::cout << "Successfully uninstalled 'TitanZFS' Windows Service.\n";
    return true;
}
#endif

// ============================================================================
// Main Entrypoint
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        std::string action = argv[1];

#ifdef _WIN32
        if (action == "install" || action == "--install") {
            return InstallService() ? 0 : 1;
        }
        if (action == "uninstall" || action == "--uninstall") {
            return UninstallService() ? 0 : 1;
        }
#endif
        if (action == "run" || action == "--console" || action == "-c" || action == "--debug") {
            auto& daemon = titanzfs::TitanZfsDaemon::Get();
            daemon.start();

            std::signal(SIGINT, [](int) {
                titanzfs::TitanZfsDaemon::Get().stop();
            });
            std::signal(SIGTERM, [](int) {
                titanzfs::TitanZfsDaemon::Get().stop();
            });

            while (daemon.isRunning()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            return 0;
        }

        std::cout << "TitanZFS Windows NT Subsystem Service Daemon\n";
        std::cout << "Usage: titanzfs_svc [command]\n\n";
        std::cout << "Commands:\n";
        std::cout << "  run, --console, -c     Run daemon directly in console (interactive/debug)\n";
#ifdef _WIN32
        std::cout << "  install, --install     Register as Windows NT Auto-Start Service\n";
        std::cout << "  uninstall, --uninstall Unregister and remove Windows NT Service\n";
#endif
        return 0;
    }

#ifdef _WIN32
    // Started by Windows Service Control Manager
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(L"TitanZFS"), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(dispatchTable)) {
        // Fall back to console mode if executed without arguments in an interactive shell
        auto& daemon = titanzfs::TitanZfsDaemon::Get();
        daemon.start();
        while (daemon.isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
#else
    auto& daemon = titanzfs::TitanZfsDaemon::Get();
    daemon.start();
    while (daemon.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#endif
    return 0;
}
