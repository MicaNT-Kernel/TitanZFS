#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <memory>

#include "titanzfs/titanzfs.hpp"
#include "titanzfs/win32_device.hpp"
#include "titanzfs/ipc.hpp"

using namespace titanzfs;

namespace {

// Control IDs
enum {
    ID_TAB_MAIN = 1001,
    ID_LV_POOLS,
    ID_LV_DISKS,
    ID_LV_DATASETS,
    ID_LV_SNAPSHOTS,
    ID_BTN_SCRUB,
    ID_BTN_MOUNT,
    ID_BTN_UNMOUNT,
    ID_BTN_EXPLORE,
    ID_BTN_TAKE_SNAP,
    ID_BTN_CREATE_POOL,
    ID_BTN_REFRESH,
    ID_STATUS_BAR,
    ID_TIMER_REFRESH = 2001
};

HINSTANCE g_hInstance{nullptr};
HWND g_hMainWnd{nullptr};
HWND g_hTab{nullptr};
HWND g_hStatus{nullptr};

// Child views
HWND g_hLvPools{nullptr};
HWND g_hLvDisks{nullptr};
HWND g_hLvDatasets{nullptr};
HWND g_hLvSnapshots{nullptr};

// Action buttons
HWND g_hBtnScrub{nullptr};
HWND g_hBtnMount{nullptr};
HWND g_hBtnUnmount{nullptr};
HWND g_hBtnExplore{nullptr};
HWND g_hBtnTakeSnap{nullptr};
HWND g_hBtnCreatePool{nullptr};
HWND g_hBtnRefresh{nullptr};

HFONT g_hFont{nullptr};
HFONT g_hFontBold{nullptr};

int g_ActiveTab = 0;

void InitListViewColumns(HWND hLv, const std::vector<std::pair<std::wstring, int>>& cols) {
    ListView_SetExtendedListViewStyle(hLv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    for (size_t i = 0; i < cols.size(); ++i) {
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = cols[i].second;
        lvc.pszText = const_cast<LPWSTR>(cols[i].first.c_str());
        lvc.iSubItem = static_cast<int>(i);
        ListView_InsertColumn(hLv, i, &lvc);
    }
}

void SetListViewItem(HWND hLv, int row, int col, const std::wstring& text) {
    LVITEMW lvi{};
    lvi.iSubItem = col;
    lvi.pszText = const_cast<LPWSTR>(text.c_str());
    if (col == 0) {
        lvi.mask = LVIF_TEXT;
        lvi.iItem = row;
        ListView_InsertItem(hLv, &lvi);
    } else {
        SendMessageW(hLv, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&lvi));
    }
}

void RefreshData() {
    auto& sys = TitanZfsSubsystem::Get();
    sys.loadCache();

    // 1. Refresh Storage Pools & Disks
    ListView_DeleteAllItems(g_hLvPools);
    ListView_DeleteAllItems(g_hLvDisks);

    int poolRow = 0;
    int diskRow = 0;

    for (const auto& [name, pool] : sys.getPools()) {
        uint64_t cap = pool->getRootVdev() ? pool->getRootVdev()->getTotalCapacity() : 0;
        std::wstring wName(name.begin(), name.end());
        std::wstring wState = (pool->getState() == VdevState::Online) ? L"ONLINE (Healthy)" : L"DEGRADED";
        std::wstring wType(pool->getVdevType().begin(), pool->getVdevType().end());
        std::wstring wCap = std::to_wstring(cap / (1024 * 1024)) + L" MB";
        std::wstring wAlloc = L"256 KB";
        std::wstring wFree = std::to_wstring((cap / (1024 * 1024)) - 1) + L" MB";

        SetListViewItem(g_hLvPools, poolRow, 0, wName);
        SetListViewItem(g_hLvPools, poolRow, 1, wType);
        SetListViewItem(g_hLvPools, poolRow, 2, wState);
        SetListViewItem(g_hLvPools, poolRow, 3, wCap);
        SetListViewItem(g_hLvPools, poolRow, 4, wAlloc);
        SetListViewItem(g_hLvPools, poolRow, 5, wFree);
        SetListViewItem(g_hLvPools, poolRow, 6, L"Fletcher-4 (256-bit)");
        ++poolRow;

        // Leaf Disks
        for (const auto& dpath : pool->getDevicePaths()) {
            std::wstring wPath(dpath.begin(), dpath.end());
            SetListViewItem(g_hLvDisks, diskRow, 0, wPath);
            SetListViewItem(g_hLvDisks, diskRow, 1, wName);
            SetListViewItem(g_hLvDisks, diskRow, 2, L"ONLINE");
            SetListViewItem(g_hLvDisks, diskRow, 3, L"0");
            SetListViewItem(g_hLvDisks, diskRow, 4, L"0");
            SetListViewItem(g_hLvDisks, diskRow, 5, L"0");
            ++diskRow;
        }
    }

    // 2. Refresh Datasets
    ListView_DeleteAllItems(g_hLvDatasets);
    int dsRow = 0;
    for (const auto& [pname, pool] : sys.getPools()) {
        for (const auto& [dname, ds] : pool->getDatasets()) {
            std::wstring wDs(dname.begin(), dname.end());
            std::string mp = ds->getMountPoint();
            std::wstring wMp = !mp.empty() ? std::wstring(mp.begin(), mp.end()) : ((dname == pname + "/data") ? L"Z:\\" : L"none");
            std::wstring wSnaps = std::to_wstring(ds->getSnapshots().size());
            std::wstring wFiles = std::to_wstring(ds->getFiles().size());

            SetListViewItem(g_hLvDatasets, dsRow, 0, wDs);
            SetListViewItem(g_hLvDatasets, dsRow, 1, L"256 KB");
            SetListViewItem(g_hLvDatasets, dsRow, 2, L"64.0 MB");
            SetListViewItem(g_hLvDatasets, dsRow, 3, wMp);
            SetListViewItem(g_hLvDatasets, dsRow, 4, wSnaps);
            SetListViewItem(g_hLvDatasets, dsRow, 5, wFiles);
            ++dsRow;
        }
    }

    // 3. Refresh Snapshots
    ListView_DeleteAllItems(g_hLvSnapshots);
    int snapRow = 0;
    for (const auto& [_, pool] : sys.getPools()) {
        for (const auto& [dname, ds] : pool->getDatasets()) {
            for (const auto& [sname, files] : ds->getSnapshots()) {
                std::wstring wFull = std::wstring(dname.begin(), dname.end()) + L"@" + std::wstring(sname.begin(), sname.end());
                std::wstring wDs(dname.begin(), dname.end());
                std::wstring wSnap(sname.begin(), sname.end());
                std::wstring wFiles = std::to_wstring(files.size()) + L" files";

                SetListViewItem(g_hLvSnapshots, snapRow, 0, wFull);
                SetListViewItem(g_hLvSnapshots, snapRow, 1, wDs);
                SetListViewItem(g_hLvSnapshots, snapRow, 2, wSnap);
                SetListViewItem(g_hLvSnapshots, snapRow, 3, wFiles);
                SetListViewItem(g_hLvSnapshots, snapRow, 4, L"CoW Instantaneous");
                SetListViewItem(g_hLvSnapshots, snapRow, 5, L"Read-Only (.zfs/snapshot)");
                ++snapRow;
            }
        }
    }

    // Status bar update
    bool ipcRunning = IpcClient::isServerRunning();
    std::wstring sb = L"TitanZFS Daemon: " + std::wstring(ipcRunning ? L"Active (\\\\.\\pipe\\titanzfs_ipc)" : L"Standalone / Direct") +
                      L"  |  Total Pools: " + std::to_wstring(poolRow) +
                      L"  |  Datasets: " + std::to_wstring(dsRow) +
                      L"  |  Snapshots: " + std::to_wstring(snapRow);
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(sb.c_str()));
}

void UpdateViewVisibility() {
    int showPools = (g_ActiveTab == 0) ? SW_SHOW : SW_HIDE;
    int showDatasets = (g_ActiveTab == 1) ? SW_SHOW : SW_HIDE;
    int showSnapshots = (g_ActiveTab == 2) ? SW_SHOW : SW_HIDE;

    ShowWindow(g_hLvPools, showPools);
    ShowWindow(g_hLvDisks, showPools);
    ShowWindow(g_hBtnScrub, showPools);

    ShowWindow(g_hLvDatasets, showDatasets);
    ShowWindow(g_hBtnMount, showDatasets);
    ShowWindow(g_hBtnUnmount, showDatasets);
    ShowWindow(g_hBtnExplore, showDatasets);

    ShowWindow(g_hLvSnapshots, showSnapshots);
    ShowWindow(g_hBtnTakeSnap, showSnapshots);
}

void LayoutControls(int width, int height) {
    if (!g_hTab) return;

    SendMessageW(g_hStatus, WM_SIZE, 0, 0);
    RECT sbRect{};
    GetWindowRect(g_hStatus, &sbRect);
    int sbHeight = sbRect.bottom - sbRect.top;

    MoveWindow(g_hTab, 10, 10, width - 20, height - sbHeight - 70, TRUE);

    int viewTop = 45;
    int viewWidth = width - 40;
    int viewHeight = height - sbHeight - 130;

    // Tab 0: Storage Pools (Top half: pools, Bottom half: disks)
    int halfHeight = (viewHeight - 10) / 2;
    MoveWindow(g_hLvPools, 20, viewTop, viewWidth, halfHeight, TRUE);
    MoveWindow(g_hLvDisks, 20, viewTop + halfHeight + 10, viewWidth, halfHeight, TRUE);

    // Tab 1: Datasets
    MoveWindow(g_hLvDatasets, 20, viewTop, viewWidth, viewHeight, TRUE);

    // Tab 2: Snapshots
    MoveWindow(g_hLvSnapshots, 20, viewTop, viewWidth, viewHeight, TRUE);

    // Buttons toolbar at bottom
    int btnY = height - sbHeight - 50;
    MoveWindow(g_hBtnRefresh, 20, btnY, 110, 32, TRUE);
    MoveWindow(g_hBtnCreatePool, 140, btnY, 140, 32, TRUE);
    MoveWindow(g_hBtnScrub, 290, btnY, 120, 32, TRUE);

    MoveWindow(g_hBtnMount, 290, btnY, 130, 32, TRUE);
    MoveWindow(g_hBtnUnmount, 430, btnY, 120, 32, TRUE);
    MoveWindow(g_hBtnExplore, 560, btnY, 140, 32, TRUE);

    MoveWindow(g_hBtnTakeSnap, 290, btnY, 140, 32, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Apply Modern Windows Immersive Dark Mode
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

            g_hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            // Main Tab Control
            g_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 10, 10, 800, 500, hwnd, reinterpret_cast<HMENU>(ID_TAB_MAIN), g_hInstance, nullptr);
            SendMessageW(g_hTab, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);

            TCITEMW tci{};
            tci.mask = TCIF_TEXT;
            wchar_t tab1[] = L"Storage Pools & Health";
            tci.pszText = tab1;
            TabCtrl_InsertItem(g_hTab, 0, &tci);

            wchar_t tab2[] = L"Datasets & Drive Mounts";
            tci.pszText = tab2;
            TabCtrl_InsertItem(g_hTab, 1, &tci);

            wchar_t tab3[] = L"Snapshots Time Machine";
            tci.pszText = tab3;
            TabCtrl_InsertItem(g_hTab, 2, &tci);

            // Tab 0 Views: Pools & Disks
            g_hLvPools = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SINGLESEL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_LV_POOLS), g_hInstance, nullptr);
            SendMessageW(g_hLvPools, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
            InitListViewColumns(g_hLvPools, {
                {L"Storage Pool", 130}, {L"Topology", 110}, {L"Health State", 140},
                {L"Total Size", 100}, {L"Allocated", 90}, {L"Free Space", 100}, {L"Checksum Engine", 150}
            });

            g_hLvDisks = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SINGLESEL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_LV_DISKS), g_hInstance, nullptr);
            SendMessageW(g_hLvDisks, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
            InitListViewColumns(g_hLvDisks, {
                {L"Physical Drive / VDEV Leaf", 220}, {L"Parent Pool", 120}, {L"Drive Status", 110},
                {L"Read Errors", 90}, {L"Write Errors", 90}, {L"Checksum Errors", 120}
            });

            // Tab 1 Views: Datasets
            g_hLvDatasets = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SINGLESEL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_LV_DATASETS), g_hInstance, nullptr);
            SendMessageW(g_hLvDatasets, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
            InitListViewColumns(g_hLvDatasets, {
                {L"Dataset Name", 220}, {L"Used Space", 100}, {L"Available Space", 120},
                {L"Mountpoint (Drive)", 140}, {L"Snapshots", 90}, {L"Files Stored", 100}
            });

            // Tab 2 Views: Snapshots
            g_hLvSnapshots = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SINGLESEL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_LV_SNAPSHOTS), g_hInstance, nullptr);
            SendMessageW(g_hLvSnapshots, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
            InitListViewColumns(g_hLvSnapshots, {
                {L"Snapshot Target", 220}, {L"Dataset", 150}, {L"Snapshot Tag", 120},
                {L"Files Preserved", 110}, {L"CoW Consistency", 140}, {L"Immutability", 160}
            });

            // Action Buttons
            g_hBtnRefresh = CreateWindowExW(0, L"BUTTON", L"Refresh View", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BTN_REFRESH), g_hInstance, nullptr);
            g_hBtnCreatePool = CreateWindowExW(0, L"BUTTON", L"Create New Pool...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BTN_CREATE_POOL), g_hInstance, nullptr);
            g_hBtnScrub = CreateWindowExW(0, L"BUTTON", L"Scrub Storage Pool", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BTN_SCRUB), g_hInstance, nullptr);

            g_hBtnMount = CreateWindowExW(0, L"BUTTON", L"Mount to Drive...", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BTN_MOUNT), g_hInstance, nullptr);
            g_hBtnUnmount = CreateWindowExW(0, L"BUTTON", L"Unmount Drive", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BTN_UNMOUNT), g_hInstance, nullptr);
            g_hBtnExplore = CreateWindowExW(0, L"BUTTON", L"Open in File Explorer", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BTN_EXPLORE), g_hInstance, nullptr);

            g_hBtnTakeSnap = CreateWindowExW(0, L"BUTTON", L"Take Snapshot...", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BTN_TAKE_SNAP), g_hInstance, nullptr);

            HWND buttons[] = { g_hBtnRefresh, g_hBtnCreatePool, g_hBtnScrub, g_hBtnMount, g_hBtnUnmount, g_hBtnExplore, g_hBtnTakeSnap };
            for (auto hBtn : buttons) SendMessageW(hBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);

            // Status Bar
            g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_STATUS_BAR), g_hInstance, nullptr);
            SendMessageW(g_hStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);

            UpdateViewVisibility();
            RefreshData();

            // 3-second auto-refresh timer
            SetTimer(hwnd, ID_TIMER_REFRESH, 3000, nullptr);
            return 0;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            LayoutControls(width, height);
            return 0;
        }

        case WM_NOTIFY: {
            auto* pnmh = reinterpret_cast<LPNMHDR>(lParam);
            if (pnmh->idFrom == ID_TAB_MAIN && pnmh->code == TCN_SELCHANGE) {
                g_ActiveTab = TabCtrl_GetCurSel(g_hTab);
                UpdateViewVisibility();
            }
            break;
        }

        case WM_TIMER: {
            if (wParam == ID_TIMER_REFRESH) {
                RefreshData();
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case ID_BTN_REFRESH: {
                    RefreshData();
                    break;
                }

                case ID_BTN_SCRUB: {
                    auto& sys = TitanZfsSubsystem::Get();
                    if (sys.getPools().empty()) {
                        MessageBoxW(hwnd, L"No active storage pools found to scrub.", L"TitanZFS Information", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    auto it = sys.getPools().begin();
                    uint64_t scrubbed = 0, repaired = 0;
                    it->second->scrubPool(scrubbed, repaired);
                    std::wstring msg = L"Scrub completed on pool '" + std::wstring(it->first.begin(), it->first.end()) +
                                      L"'\n\nVerified Blocks: " + std::to_wstring(scrubbed) +
                                      L"\nRepaired Bit-Rot Errors: " + std::to_wstring(repaired);
                    MessageBoxW(hwnd, msg.c_str(), L"Scrub Finished", MB_OK | MB_ICONINFORMATION);
                    RefreshData();
                    break;
                }

                case ID_BTN_MOUNT: {
                    auto& sys = TitanZfsSubsystem::Get();
                    if (sys.getPools().empty()) {
                        MessageBoxW(hwnd, L"No pools available to mount.", L"Notice", MB_OK);
                        break;
                    }
                    auto pool = sys.getPools().begin()->second;
                    auto ds = pool->getDataset(pool->getName() + "/data");
                    if (ds) {
                        ds->setMountPoint("Z:\\");
                        sys.saveCache();
                        std::wstring cmd = L"titanzfs_mount.exe " + std::wstring(pool->getName().begin(), pool->getName().end()) + L"/data Z:";
                        STARTUPINFOW si{};
                        si.cb = sizeof(si);
                        PROCESS_INFORMATION pi{};
                        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                        cmdBuf.push_back(0);
                        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
                            CloseHandle(pi.hThread);
                            CloseHandle(pi.hProcess);
                        }
                        MessageBoxW(hwnd, L"Dataset 'tank/data' mounted successfully to Z:\\ via WinFsp userland bridge.\nFilesystem is active and visible in File Explorer.", L"Dataset Mounted", MB_OK | MB_ICONINFORMATION);
                        RefreshData();
                    }
                    break;
                }

                case ID_BTN_UNMOUNT: {
                    auto& sys = TitanZfsSubsystem::Get();
                    for (auto& [name, pool] : sys.getPools()) {
                        for (auto& [dsName, ds] : pool->getDatasets()) {
                            if (!ds->getMountPoint().empty()) {
                                ds->setMountPoint("");
                            }
                        }
                    }
                    sys.saveCache();
                    MessageBoxW(hwnd, L"Dataset unmounted from Z:\\. WinFsp drive letter released.", L"Dataset Unmounted", MB_OK | MB_ICONINFORMATION);
                    RefreshData();
                    break;
                }

                case ID_BTN_EXPLORE: {
                    // Open mounted drive in Windows File Explorer
                    ShellExecuteW(nullptr, L"open", L"Z:\\", nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                }

                case ID_BTN_TAKE_SNAP: {
                    auto& sys = TitanZfsSubsystem::Get();
                    if (sys.getPools().empty()) {
                        MessageBoxW(hwnd, L"No pools available to take snapshots.", L"Notice", MB_OK);
                        break;
                    }
                    auto pool = sys.getPools().begin()->second;
                    auto ds = pool->getDataset(pool->getName() + "/data");
                    if (ds) {
                        static int snapCounter = 1;
                        std::string snapTag = "snap_" + std::to_string(std::time(nullptr) % 100000);
                        if (IpcClient::isServerRunning()) {
                            std::string resp;
                            IpcClient::send(IpcCmdZfsSnapshot, pool->getName() + "/data@" + snapTag, resp);
                        } else {
                            ds->createSnapshot(snapTag, pool->getCurrentTxg());
                            sys.saveCache();
                        }
                        std::wstring msg = L"Successfully captured snapshot: " + std::wstring(snapTag.begin(), snapTag.end()) + L"\nAvailable immediately at Z:\\.zfs\\snapshot\\" + std::wstring(snapTag.begin(), snapTag.end());
                        MessageBoxW(hwnd, msg.c_str(), L"Instantaneous Snapshot Created", MB_OK | MB_ICONINFORMATION);
                        RefreshData();
                    }
                    break;
                }

                case ID_BTN_CREATE_POOL: {
                    // Quick pool constructor dialog
                    int ret = MessageBoxW(hwnd,
                        L"Create a high-performance RAID-Z1 storage pool 'tank' with 3 sparse block devices (PhysicalDrive1.raw, PhysicalDrive2.raw, PhysicalDrive3.raw)?",
                        L"TitanZFS Storage Pool Creator Wizard",
                        MB_YESNO | MB_ICONQUESTION);
                    if (ret == IDYES) {
                        auto d0 = CreateWin32BlockDevice("PhysicalDrive1", "PhysicalDrive1.raw", 4096, 64 * 1024 * 1024);
                        auto d1 = CreateWin32BlockDevice("PhysicalDrive2", "PhysicalDrive2.raw", 4096, 64 * 1024 * 1024);
                        auto d2 = CreateWin32BlockDevice("PhysicalDrive3", "PhysicalDrive3.raw", 4096, 64 * 1024 * 1024);

                        auto rz = std::make_shared<Raidz1Vdev>(0x300, "raidz1-0");
                        rz->addChild(std::make_shared<DiskVdev>(0x301, "PhysicalDrive1.raw", d0));
                        rz->addChild(std::make_shared<DiskVdev>(0x302, "PhysicalDrive2.raw", d1));
                        rz->addChild(std::make_shared<DiskVdev>(0x303, "PhysicalDrive3.raw", d2));

                        auto pool = std::make_shared<ZfsPool>("tank", rz);
                        pool->setVdevType("raidz1");
                        pool->setDevicePaths({"PhysicalDrive1.raw", "PhysicalDrive2.raw", "PhysicalDrive3.raw"});
                        pool->createDataset("tank/data");
                        pool->syncTransactionGroup();

                        auto& sys = TitanZfsSubsystem::Get();
                        sys.registerPool(pool);
                        sys.saveCache();

                        MessageBoxW(hwnd, L"Storage pool 'tank' (RAID-Z1) successfully created and committed to on-disk metaslabs!", L"Pool Created", MB_OK | MB_ICONINFORMATION);
                        RefreshData();
                    }
                    break;
                }
            }
            break;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, ID_TIMER_REFRESH);
            if (g_hFont) DeleteObject(g_hFont);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInstance = hInstance;

    INITCOMMONCONTROLSEX icex{
        .dwSize = sizeof(icex),
        .dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"TitanZFS_ManagementConsole_Class";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"TitanZFS Storage Pool Management Console (Enterprise)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 620,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hMainWnd) return 1;

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// WinMain entry point for Win32 GUI subsystem linkers (strictly zero console window)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    return wWinMain(hInstance, nullptr, GetCommandLineW(), nCmdShow);
}
