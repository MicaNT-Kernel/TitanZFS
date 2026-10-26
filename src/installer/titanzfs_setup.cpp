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
#include <winsvc.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <algorithm>
#include "titanzfs/compression.hpp"

namespace {

#pragma pack(push, 1)
struct PayloadFooter {
    uint64_t tocOffset{0};
    uint64_t payloadStart{0};
    char     magic[16]{"TITANZFS_BUNDLE"};
};
#pragma pack(pop)
static_assert(sizeof(PayloadFooter) == 32, "PayloadFooter must be 32 bytes");

struct EmbeddedFile {
    std::string name;
    uint64_t offset{0};
    uint64_t compSize{0};
    uint64_t uncompSize{0};
    uint32_t isCompressed{0};
};

bool ReadEmbeddedPayload(std::vector<EmbeddedFile>& outEntries) {
    wchar_t thisExe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, thisExe, MAX_PATH) == 0) return false;

    std::ifstream file(thisExe, std::ios::binary);
    if (!file.is_open()) return false;

    file.seekg(0, std::ios::end);
    uint64_t fileSize = static_cast<uint64_t>(file.tellg());
    if (fileSize < sizeof(PayloadFooter)) return false;

    file.seekg(fileSize - sizeof(PayloadFooter), std::ios::beg);
    PayloadFooter footer{};
    file.read(reinterpret_cast<char*>(&footer), sizeof(footer));

    if (std::memcmp(footer.magic, "TITANZFS_BUNDLE", 15) != 0) return false;
    if (footer.tocOffset >= fileSize || footer.payloadStart >= fileSize) return false;

    file.seekg(footer.tocOffset, std::ios::beg);
    uint32_t entryCount = 0;
    file.read(reinterpret_cast<char*>(&entryCount), sizeof(entryCount));
    if (entryCount == 0 || entryCount > 256) return false;

    outEntries.clear();
    for (uint32_t i = 0; i < entryCount; ++i) {
        uint32_t nameLen = 0;
        file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (nameLen > 1024) return false;
        std::string name(nameLen, '\0');
        file.read(name.data(), nameLen);

        EmbeddedFile ent;
        ent.name = name;
        file.read(reinterpret_cast<char*>(&ent.offset), sizeof(ent.offset));
        file.read(reinterpret_cast<char*>(&ent.compSize), sizeof(ent.compSize));
        file.read(reinterpret_cast<char*>(&ent.uncompSize), sizeof(ent.uncompSize));
        file.read(reinterpret_cast<char*>(&ent.isCompressed), sizeof(ent.isCompressed));
        outEntries.push_back(std::move(ent));
    }
    return !outEntries.empty();
}

bool ExtractEmbeddedFile(const EmbeddedFile& ent, const std::wstring& outPath) {
    wchar_t thisExe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, thisExe, MAX_PATH) == 0) return false;

    std::ifstream file(thisExe, std::ios::binary);
    if (!file.is_open()) return false;

    file.seekg(ent.offset, std::ios::beg);
    std::vector<uint8_t> buffer(ent.compSize);
    file.read(reinterpret_cast<char*>(buffer.data()), ent.compSize);

    std::vector<uint8_t> decompressed;
    if (ent.isCompressed) {
        decompressed.resize(ent.uncompSize);
        if (!titanzfs::CompressionEngine::DecompressLZ4(buffer, decompressed)) {
            return false;
        }
    } else {
        decompressed = std::move(buffer);
    }

    std::error_code ec;
    std::filesystem::path p(outPath);
    std::filesystem::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
    return true;
}

enum {
    ID_EDIT_DEST = 1001,
    ID_BTN_BROWSE,
    ID_CHK_PATH,
    ID_CHK_SERVICE,
    ID_CHK_WINFSP,
    ID_CHK_STARTMENU,
    ID_CHK_DESKTOP,
    ID_CHK_LAUNCH,
    ID_PROGRESS,
    ID_STATIC_STATUS,
    ID_BTN_INSTALL,
    ID_BTN_CANCEL
};

HINSTANCE g_hInstance{nullptr};
HWND g_hMainWnd{nullptr};
HWND g_hEditDest{nullptr};
HWND g_hBtnBrowse{nullptr};
HWND g_hChkPath{nullptr};
HWND g_hChkService{nullptr};
HWND g_hChkWinFsp{nullptr};
HWND g_hChkStartMenu{nullptr};
HWND g_hChkDesktop{nullptr};
HWND g_hChkLaunch{nullptr};
HWND g_hProgress{nullptr};
HWND g_hStaticStatus{nullptr};
HWND g_hBtnInstall{nullptr};
HWND g_hBtnCancel{nullptr};

bool IsWinFspInstalled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WinFsp", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

HFONT g_hFontTitle{nullptr};
HFONT g_hFontSub{nullptr};
HFONT g_hFontNormal{nullptr};
HFONT g_hFontBold{nullptr};
HBRUSH g_hDarkBrush{nullptr};

ULONG_PTR g_gdiplusToken{0};
std::wstring g_HeroImagePath;
bool g_InstallFinished{false};

std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ws(path);
    size_t pos = ws.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? ws.substr(0, pos) : L".";
}

std::wstring FindHeroImage() {
    // 1. Check if embedded inside this executable's payload
    std::vector<EmbeddedFile> entries;
    if (ReadEmbeddedPayload(entries)) {
        for (const auto& e : entries) {
            if (e.name.find("hero.jpg") != std::string::npos) {
                std::error_code ec;
                std::wstring tempDir = std::filesystem::temp_directory_path().wstring() + L"\\TitanZFS";
                std::filesystem::create_directories(tempDir, ec);
                std::wstring tempHero = tempDir + L"\\titanzfs_hero.jpg";
                if (!std::filesystem::exists(tempHero, ec)) {
                    ExtractEmbeddedFile(e, tempHero);
                }
                if (std::filesystem::exists(tempHero, ec)) {
                    return tempHero;
                }
            }
        }
    }

    // 2. Fall back to local directory scan
    std::wstring dir = GetModuleDir();
    std::vector<std::wstring> candidates = {
        dir + L"\\assets\\titanzfs_hero.jpg",
        dir + L"\\..\\assets\\titanzfs_hero.jpg",
        dir + L"\\titanzfs_hero.jpg",
        L"assets\\titanzfs_hero.jpg"
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return L"";
}

// Proportional Aspect-Ratio Filling Helper (Zero distortion, perfectly centered)
void DrawAspectFill(Gdiplus::Graphics& graphics, Gdiplus::Image* pImg, float targetX, float targetY, float targetW, float targetH) {
    if (!pImg) return;
    float imgW = static_cast<float>(pImg->GetWidth());
    float imgH = static_cast<float>(pImg->GetHeight());
    if (imgW <= 0.0f || imgH <= 0.0f) return;

    float scale = (std::max)(targetW / imgW, targetH / imgH);
    float drawW = imgW * scale;
    float drawH = imgH * scale;
    float drawX = targetX + (targetW - drawW) / 2.0f;
    float drawY = targetY + (targetH - drawH) / 2.0f;

    Gdiplus::Region oldClip;
    graphics.GetClip(&oldClip);
    graphics.SetClip(Gdiplus::RectF(targetX, targetY, targetW, targetH));
    graphics.DrawImage(pImg, drawX, drawY, drawW, drawH);
    graphics.SetClip(&oldClip);
}

bool CreateShortcut(const std::wstring& targetPath, const std::wstring& shortcutPath, const std::wstring& description) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&psl));
    if (SUCCEEDED(hr)) {
        psl->SetPath(targetPath.c_str());
        psl->SetDescription(description.c_str());
        psl->SetIconLocation(targetPath.c_str(), 0);
        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
        if (SUCCEEDED(hr)) {
            hr = ppf->Save(shortcutPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    return SUCCEEDED(hr);
}

bool AddToSystemPath(const std::wstring& dir) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        wchar_t currentPath[32768]{};
        DWORD bufSize = sizeof(currentPath);
        DWORD type = REG_EXPAND_SZ;
        RegQueryValueExW(hKey, L"Path", nullptr, &type, reinterpret_cast<LPBYTE>(currentPath), &bufSize);
        std::wstring ws(currentPath);
        if (ws.find(dir) == std::wstring::npos) {
            if (!ws.empty() && ws.back() != L';') ws += L';';
            ws += dir;
            RegSetValueExW(hKey, L"Path", 0, type, reinterpret_cast<const BYTE*>(ws.c_str()), static_cast<DWORD>((ws.size() + 1) * sizeof(wchar_t)));
            DWORD_PTR dwResult;
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 2000, &dwResult);
        }
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool RemoveFromSystemPath(const std::wstring& dir) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        wchar_t currentPath[32768]{};
        DWORD bufSize = sizeof(currentPath);
        DWORD type = REG_EXPAND_SZ;
        RegQueryValueExW(hKey, L"Path", nullptr, &type, reinterpret_cast<LPBYTE>(currentPath), &bufSize);
        std::wstring ws(currentPath);
        size_t pos = ws.find(dir);
        if (pos != std::wstring::npos) {
            size_t len = dir.length();
            if (pos + len < ws.length() && ws[pos + len] == L';') len++;
            else if (pos > 0 && ws[pos - 1] == L';') { pos--; len++; }
            ws.erase(pos, len);
            RegSetValueExW(hKey, L"Path", 0, type, reinterpret_cast<const BYTE*>(ws.c_str()), static_cast<DWORD>((ws.size() + 1) * sizeof(wchar_t)));
            DWORD_PTR dwResult;
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 2000, &dwResult);
        }
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool RegisterUninstallEntry(const std::wstring& installDir) {
    HKEY hKey;
    const wchar_t subkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TitanZFS";
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        auto setStr = [&](const wchar_t* valName, const std::wstring& val) {
            RegSetValueExW(hKey, valName, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        };
        auto setDword = [&](const wchar_t* valName, DWORD val) {
            RegSetValueExW(hKey, valName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        };

        setStr(L"DisplayName", L"TitanZFS Storage Pool Subsystem");
        setStr(L"DisplayVersion", L"1.0.0");
        setStr(L"Publisher", L"MicaNT-Kernel / Daniel Elliott");
        setStr(L"InstallLocation", installDir);
        setStr(L"UninstallString", L"\"" + installDir + L"\\titanzfs_setup.exe\" /uninstall");
        setStr(L"QuietUninstallString", L"\"" + installDir + L"\\titanzfs_setup.exe\" /uninstall /silent");
        setStr(L"DisplayIcon", installDir + L"\\titanzfs_admin.exe,0");
        setDword(L"EstimatedSize", 14000);
        setDword(L"NoModify", 1);
        setDword(L"NoRepair", 1);

        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool UnregisterUninstallEntry() {
    return RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TitanZFS") == ERROR_SUCCESS;
}

bool CopyAppFile(const std::wstring& srcDir, const std::wstring& dstDir, const std::wstring& fileName) {
    std::vector<std::wstring> candidates = {
        srcDir + L"\\" + fileName,
        srcDir + L"\\bin\\" + fileName,
        srcDir + L"\\..\\build\\" + fileName
    };
    std::wstring foundSrc;
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) {
            foundSrc = c;
            break;
        }
    }
    if (foundSrc.empty()) return false;

    std::wstring dst = dstDir + L"\\" + fileName;
    std::error_code ec;
    std::filesystem::create_directories(dstDir, ec);
    return std::filesystem::copy_file(foundSrc, dst, std::filesystem::copy_options::overwrite_existing, ec);
}

// Perform Installation Task
bool PerformInstall(const std::wstring& dstDir, bool addPath, bool installSvc, bool installWinFsp, bool startMenu, bool desktop, HWND hProg, HWND hStat) {
    auto setStatus = [&](const std::wstring& text, int pos) {
        if (hStat) SetWindowTextW(hStat, text.c_str());
        if (hProg) SendMessageW(hProg, PBM_SETPOS, pos, 0);
    };

    setStatus(L"Preparing destination directory...", 10);
    std::error_code ec;
    std::filesystem::create_directories(dstDir, ec);

    std::wstring srcDir = GetModuleDir();

    // 1. Install WinFsp Filesystem Driver if requested and missing
    if (installWinFsp && !IsWinFspInstalled()) {
        setStatus(L"Installing WinFsp Filesystem Driver (WHQL)...", 15);
        std::wstring tempMsi = std::filesystem::temp_directory_path().wstring() + L"\\TitanZFS\\winfsp.msi";
        bool foundMsi = false;

        std::vector<EmbeddedFile> entries;
        if (ReadEmbeddedPayload(entries)) {
            for (const auto& ef : entries) {
                if (ef.name.find("winfsp") != std::string::npos && ef.name.ends_with(".msi")) {
                    ExtractEmbeddedFile(ef, tempMsi);
                    foundMsi = std::filesystem::exists(tempMsi, ec);
                    break;
                }
            }
        }

        if (!foundMsi) {
            std::wstring localMsi = srcDir + L"\\redist\\winfsp-2.1.25156.msi";
            if (std::filesystem::exists(localMsi, ec)) {
                tempMsi = localMsi;
                foundMsi = true;
            }
        }

        if (foundMsi) {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"msiexec.exe /i \"" + tempMsi + L"\" /qn /norestart";
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(0);
            if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 60000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            if (tempMsi.find(L"TitanZFS") != std::string::npos) {
                std::filesystem::remove(tempMsi, ec);
            }
        }
    }

    // 2. Install Executables and Assets (from Embedded Bundle or Loose Files)
    std::vector<EmbeddedFile> embeddedFiles;
    bool hasEmbedded = ReadEmbeddedPayload(embeddedFiles);

    if (hasEmbedded && !embeddedFiles.empty()) {
        int idx = 0;
        for (const auto& ef : embeddedFiles) {
            int pct = 25 + static_cast<int>((idx * 40) / embeddedFiles.size());
            std::wstring wName(ef.name.begin(), ef.name.end());
            setStatus(L"Extracting " + wName + L"...", pct);

            std::wstring targetFilePath = dstDir + L"\\" + wName;
            ExtractEmbeddedFile(ef, targetFilePath);
            idx++;
        }
    } else {
        // Fallback to copying loose files from installer directory
        setStatus(L"Installing TitanZFS Management Console...", 25);
        CopyAppFile(srcDir, dstDir, L"titanzfs_admin.exe");

        setStatus(L"Installing Windows NT Background Service daemon...", 40);
        CopyAppFile(srcDir, dstDir, L"titanzfs_svc.exe");

        setStatus(L"Installing WinFsp Userland Filesystem Bridge...", 50);
        CopyAppFile(srcDir, dstDir, L"titanzfs_mount.exe");

        setStatus(L"Installing ZFS and Storage Pool Command-Line Tools...", 60);
        CopyAppFile(srcDir, dstDir, L"zpool.exe");
        CopyAppFile(srcDir, dstDir, L"zfs.exe");

        // Copy assets if available
        std::wstring heroSrc = FindHeroImage();
        if (!heroSrc.empty()) {
            std::filesystem::create_directories(dstDir + L"\\assets", ec);
            std::filesystem::copy_file(heroSrc, dstDir + L"\\assets\\titanzfs_hero.jpg", std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    // Copy setup tool itself for uninstaller capability
    wchar_t thisExe[MAX_PATH];
    GetModuleFileNameW(nullptr, thisExe, MAX_PATH);
    std::filesystem::copy_file(thisExe, dstDir + L"\\titanzfs_setup.exe", std::filesystem::copy_options::overwrite_existing, ec);

    // 3. Add to PATH
    if (addPath) {
        setStatus(L"Configuring System PATH environment variables...", 70);
        AddToSystemPath(dstDir);
    }

    // 3. Register & Start Service
    if (installSvc) {
        setStatus(L"Registering TitanZFS Windows NT Auto-Start Service...", 80);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + dstDir + L"\\titanzfs_svc.exe\" install";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(0);
        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        // Start service
        SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (hSCM) {
            SC_HANDLE hSvc = OpenServiceW(hSCM, L"TitanZFS", SERVICE_START);
            if (hSvc) {
                StartServiceW(hSvc, 0, nullptr);
                CloseServiceHandle(hSvc);
            }
            CloseServiceHandle(hSCM);
        }
    }

    // 4. Shortcuts
    std::wstring adminPath = dstDir + L"\\titanzfs_admin.exe";
    if (startMenu) {
        setStatus(L"Creating Start Menu shortcuts...", 90);
        PWSTR pszPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &pszPath))) {
            std::wstring folder = std::wstring(pszPath) + L"\\TitanZFS";
            CoTaskMemFree(pszPath);
            std::filesystem::create_directories(folder, ec);
            CreateShortcut(adminPath, folder + L"\\TitanZFS Management Console.lnk", L"TitanZFS Management Console");
        }
    }

    if (desktop) {
        setStatus(L"Creating Desktop shortcut...", 95);
        PWSTR pszPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_PublicDesktop, 0, nullptr, &pszPath))) {
            std::wstring folder = std::wstring(pszPath);
            CoTaskMemFree(pszPath);
            CreateShortcut(adminPath, folder + L"\\TitanZFS Management Console.lnk", L"TitanZFS Management Console");
        }
    }

    // 5. Register Add/Remove Programs
    RegisterUninstallEntry(dstDir);

    setStatus(L"Installation complete! TitanZFS is ready for use.", 100);
    return true;
}

// Perform Uninstall Task
bool PerformUninstall(bool silent) {
    if (!silent) {
        int ret = MessageBoxW(nullptr,
            L"Are you sure you want to uninstall TitanZFS and remove all associated system services?",
            L"TitanZFS Uninstall",
            MB_YESNO | MB_ICONQUESTION);
        if (ret != IDYES) return false;
    }

    std::wstring installDir = GetModuleDir();

    // 1. Stop and remove Service
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (hSCM) {
        SC_HANDLE hSvc = OpenServiceW(hSCM, L"TitanZFS", SERVICE_STOP | DELETE);
        if (hSvc) {
            SERVICE_STATUS ss{};
            ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
            DeleteService(hSvc);
            CloseServiceHandle(hSvc);
        }
        CloseServiceHandle(hSCM);
    }

    // 2. Remove from PATH
    RemoveFromSystemPath(installDir);

    // 3. Remove Shortcuts
    PWSTR pszPrograms = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &pszPrograms))) {
        std::error_code ec;
        std::filesystem::remove_all(std::wstring(pszPrograms) + L"\\TitanZFS", ec);
        CoTaskMemFree(pszPrograms);
    }

    PWSTR pszDesktop = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_PublicDesktop, 0, nullptr, &pszDesktop))) {
        std::error_code ec;
        std::filesystem::remove(std::wstring(pszDesktop) + L"\\TitanZFS Management Console.lnk", ec);
        CoTaskMemFree(pszDesktop);
    }

    // 4. Remove Add/Remove Programs registry key
    UnregisterUninstallEntry();

    // 5. Delete files in directory
    std::error_code ec;
    std::vector<std::wstring> files = {
        L"titanzfs_admin.exe", L"titanzfs_svc.exe", L"titanzfs_mount.exe",
        L"zpool.exe", L"zfs.exe", L"zpool.cache"
    };
    for (const auto& f : files) {
        std::filesystem::remove(installDir + L"\\" + f, ec);
    }
    std::filesystem::remove_all(installDir + L"\\assets", ec);

    // Schedule deletion of the uninstaller itself on next reboot if locked
    wchar_t thisExe[MAX_PATH];
    GetModuleFileNameW(nullptr, thisExe, MAX_PATH);
    MoveFileExW(thisExe, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    if (!silent) {
        MessageBoxW(nullptr,
            L"TitanZFS has been successfully uninstalled from this system.",
            L"TitanZFS Uninstall",
            MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

// Main Window Procedure (Left-Sidebar Hero Layout)
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

            g_hFontTitle = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontSub = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hDarkBrush = CreateSolidBrush(RGB(22, 22, 24));

            g_HeroImagePath = FindHeroImage();

            // Right Content Area (Starts at x = 290, width = 500)
            HWND hTitle = CreateWindowExW(0, L"STATIC", L"TitanZFS Storage Subsystem", WS_CHILD | WS_VISIBLE, 290, 22, 500, 28, hwnd, nullptr, g_hInstance, nullptr);
            SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontTitle), TRUE);

            HWND hSub = CreateWindowExW(0, L"STATIC", L"Native ZFS Storage Engine for Windows", WS_CHILD | WS_VISIBLE, 290, 52, 500, 20, hwnd, nullptr, g_hInstance, nullptr);
            SendMessageW(hSub, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontSub), TRUE);

            HWND hDesc = CreateWindowExW(0, L"STATIC", L"Create and manage ZFS storage pools, RAID-Z1 arrays, and datasets.", WS_CHILD | WS_VISIBLE, 290, 75, 500, 20, hwnd, nullptr, g_hInstance, nullptr);
            SendMessageW(hDesc, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

            // Destination Folder (y = 115)
            HWND hLblDest = CreateWindowExW(0, L"STATIC", L"Installation Destination:", WS_CHILD | WS_VISIBLE, 290, 115, 200, 18, hwnd, nullptr, g_hInstance, nullptr);
            SendMessageW(hLblDest, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);

            g_hEditDest = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"C:\\Program Files\\TitanZFS", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 290, 138, 385, 28, hwnd, reinterpret_cast<HMENU>(ID_EDIT_DEST), g_hInstance, nullptr);
            SendMessageW(g_hEditDest, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

            g_hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 685, 137, 95, 30, hwnd, reinterpret_cast<HMENU>(ID_BTN_BROWSE), g_hInstance, nullptr);
            SendMessageW(g_hBtnBrowse, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

            // Spaced Checkboxes (y = 180 to 320)
            bool winfspPresent = IsWinFspInstalled();
            std::wstring winfspLabel = winfspPresent ? 
                L"WinFsp Filesystem Driver (Detected & Configured)" : 
                L"Install WinFsp Filesystem Driver (Required for drive letters)";

            g_hChkPath = CreateWindowExW(0, L"BUTTON", L"Add TitanZFS to System PATH (zpool & zfs CLI access)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 290, 180, 490, 22, hwnd, reinterpret_cast<HMENU>(ID_CHK_PATH), g_hInstance, nullptr);
            SendMessageW(g_hChkPath, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
            SendMessageW(g_hChkPath, BM_SETCHECK, BST_CHECKED, 0);

            g_hChkService = CreateWindowExW(0, L"BUTTON", L"Register & Start TitanZFS Windows NT Service (SERVICE_AUTO_START)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 290, 206, 490, 22, hwnd, reinterpret_cast<HMENU>(ID_CHK_SERVICE), g_hInstance, nullptr);
            SendMessageW(g_hChkService, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
            SendMessageW(g_hChkService, BM_SETCHECK, BST_CHECKED, 0);

            DWORD winfspStyle = WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | (winfspPresent ? WS_DISABLED : WS_TABSTOP);
            g_hChkWinFsp = CreateWindowExW(0, L"BUTTON", winfspLabel.c_str(), winfspStyle, 290, 232, 490, 22, hwnd, reinterpret_cast<HMENU>(ID_CHK_WINFSP), g_hInstance, nullptr);
            SendMessageW(g_hChkWinFsp, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
            SendMessageW(g_hChkWinFsp, BM_SETCHECK, BST_CHECKED, 0);

            g_hChkStartMenu = CreateWindowExW(0, L"BUTTON", L"Create Start Menu Program shortcut", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 290, 258, 490, 22, hwnd, reinterpret_cast<HMENU>(ID_CHK_STARTMENU), g_hInstance, nullptr);
            SendMessageW(g_hChkStartMenu, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
            SendMessageW(g_hChkStartMenu, BM_SETCHECK, BST_CHECKED, 0);

            g_hChkDesktop = CreateWindowExW(0, L"BUTTON", L"Create Desktop shortcut", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 290, 284, 490, 22, hwnd, reinterpret_cast<HMENU>(ID_CHK_DESKTOP), g_hInstance, nullptr);
            SendMessageW(g_hChkDesktop, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
            SendMessageW(g_hChkDesktop, BM_SETCHECK, BST_CHECKED, 0);

            g_hChkLaunch = CreateWindowExW(0, L"BUTTON", L"Launch TitanZFS Management Console after installation", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 290, 310, 490, 22, hwnd, reinterpret_cast<HMENU>(ID_CHK_LAUNCH), g_hInstance, nullptr);
            SendMessageW(g_hChkLaunch, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
            SendMessageW(g_hChkLaunch, BM_SETCHECK, BST_CHECKED, 0);

            // Progress & Status (y = 365)
            g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 290, 365, 490, 16, hwnd, reinterpret_cast<HMENU>(ID_PROGRESS), g_hInstance, nullptr);
            SendMessageW(g_hProgress, PBM_SETRANGE32, 0, 100);
            SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

            g_hStaticStatus = CreateWindowExW(0, L"STATIC", L"Ready to install TitanZFS on Windows.", WS_CHILD | WS_VISIBLE, 290, 392, 490, 42, hwnd, reinterpret_cast<HMENU>(ID_STATIC_STATUS), g_hInstance, nullptr);
            SendMessageW(g_hStaticStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

            // Action Buttons (y = 465)
            g_hBtnInstall = CreateWindowExW(0, L"BUTTON", L"Install TitanZFS", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 485, 465, 175, 38, hwnd, reinterpret_cast<HMENU>(ID_BTN_INSTALL), g_hInstance, nullptr);
            SendMessageW(g_hBtnInstall, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);

            g_hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 675, 465, 105, 38, hwnd, reinterpret_cast<HMENU>(ID_BTN_CANCEL), g_hInstance, nullptr);
            SendMessageW(g_hBtnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int height = clientRect.bottom - clientRect.top;
            int width = clientRect.right - clientRect.left;

            // Draw Left Hero Sidebar Panel (0, 0, 260, height) with Aspect-Fill
            if (!g_HeroImagePath.empty()) {
                Gdiplus::Graphics graphics(hdc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                Gdiplus::Image heroImg(g_HeroImagePath.c_str());
                DrawAspectFill(graphics, &heroImg, 0.0f, 0.0f, 260.0f, static_cast<float>(height));

                // Bottom subtle dark vignette overlay
                Gdiplus::SolidBrush vignette(Gdiplus::Color(220, 10, 14, 20));
                graphics.FillRectangle(&vignette, 0, height - 70, 260, 70);

                Gdiplus::FontFamily ff(L"Segoe UI");
                Gdiplus::Font fTitle(&ff, 10.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                Gdiplus::Font fSub(&ff, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush tBrush(Gdiplus::Color(255, 230, 240, 255));
                Gdiplus::SolidBrush sBrush(Gdiplus::Color(220, 0, 200, 255));

                graphics.DrawString(L"TitanZFS v1.0.0", -1, &fTitle, Gdiplus::PointF(16.0f, static_cast<Gdiplus::REAL>(height - 52)), &tBrush);
                graphics.DrawString(L"Windows Edition", -1, &fSub, Gdiplus::PointF(16.0f, static_cast<Gdiplus::REAL>(height - 30)), &sBrush);
            } else {
                HBRUSH hSideBrush = CreateSolidBrush(RGB(15, 23, 35));
                RECT rcSide{0, 0, 260, height};
                FillRect(hdc, &rcSide, hSideBrush);
                DeleteObject(hSideBrush);
            }

            // Vertical Cyan Accent Separator Line at x = 260
            HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 190, 255));
            HPEN hOldPen = reinterpret_cast<HPEN>(SelectObject(hdc, hPen));
            MoveToEx(hdc, 260, 0, nullptr);
            LineTo(hdc, 260, height);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORDLG: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, RGB(22, 22, 24));
            SetTextColor(hdc, RGB(225, 225, 225));
            return reinterpret_cast<LRESULT>(g_hDarkBrush);
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == ID_BTN_CANCEL) {
                PostQuitMessage(0);
            } else if (wmId == ID_BTN_BROWSE) {
                IFileDialog* pfd = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                    DWORD dwOptions = 0;
                    if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
                        pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
                    }
                    if (SUCCEEDED(pfd->Show(hwnd))) {
                        IShellItem* psi = nullptr;
                        if (SUCCEEDED(pfd->GetResult(&psi))) {
                            PWSTR pszPath = nullptr;
                            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                                SetWindowTextW(g_hEditDest, pszPath);
                                CoTaskMemFree(pszPath);
                            }
                            psi->Release();
                        }
                    }
                    pfd->Release();
                }
            } else if (wmId == ID_BTN_INSTALL) {
                if (g_InstallFinished) {
                    wchar_t destBuf[MAX_PATH]{};
                    GetWindowTextW(g_hEditDest, destBuf, MAX_PATH);
                    std::wstring adminExe = std::wstring(destBuf) + L"\\titanzfs_admin.exe";
                    ShellExecuteW(nullptr, L"open", adminExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    PostQuitMessage(0);
                    return 0;
                }

                wchar_t destBuf[MAX_PATH]{};
                GetWindowTextW(g_hEditDest, destBuf, MAX_PATH);
                std::wstring dstDir(destBuf);

                bool addPath = (SendMessageW(g_hChkPath, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool installSvc = (SendMessageW(g_hChkService, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool installWinFsp = (SendMessageW(g_hChkWinFsp, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool startMenu = (SendMessageW(g_hChkStartMenu, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool desktop = (SendMessageW(g_hChkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool launchAdmin = (SendMessageW(g_hChkLaunch, BM_GETCHECK, 0, 0) == BST_CHECKED);

                EnableWindow(g_hBtnInstall, FALSE);
                EnableWindow(g_hBtnBrowse, FALSE);
                EnableWindow(g_hEditDest, FALSE);

                // Run install in worker thread
                std::thread([=]() {
                    PerformInstall(dstDir, addPath, installSvc, installWinFsp, startMenu, desktop, g_hProgress, g_hStaticStatus);
                    g_InstallFinished = true;
                    if (launchAdmin) {
                        SetWindowTextW(g_hBtnInstall, L"Finish & Launch");
                    } else {
                        SetWindowTextW(g_hBtnInstall, L"Finish");
                    }
                    EnableWindow(g_hBtnInstall, TRUE);
                    ShowWindow(g_hBtnCancel, SW_HIDE);
                }).detach();
            }
            break;
        }

        case WM_DESTROY: {
            if (g_hFontTitle) DeleteObject(g_hFontTitle);
            if (g_hFontSub) DeleteObject(g_hFontSub);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            if (g_hDarkBrush) DeleteObject(g_hDarkBrush);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int g_SplashProgress = 0;

LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            SetTimer(hwnd, 1, 20, nullptr); // 50 ticks * 20ms = ~1.2s total splash duration
            return 0;
        }

        case WM_TIMER: {
            if (wParam == 1) {
                g_SplashProgress += 2;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (g_SplashProgress >= 100) {
                    KillTimer(hwnd, 1);
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // Prevent background flicker

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Off-screen memory buffer for 100% flicker-free rendering
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBmp = reinterpret_cast<HBITMAP>(SelectObject(memDC, memBmp));

            {
                Gdiplus::Graphics graphics(memDC);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

                if (!g_HeroImagePath.empty()) {
                    Gdiplus::Image heroImg(g_HeroImagePath.c_str());
                    DrawAspectFill(graphics, &heroImg, 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                } else {
                    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 15, 23, 35));
                    graphics.FillRectangle(&bgBrush, 0, 0, w, h);
                }

                // Semi-transparent bottom status banner (48px)
                Gdiplus::SolidBrush overlayBrush(Gdiplus::Color(215, 10, 14, 22));
                graphics.FillRectangle(&overlayBrush, 0, h - 48, w, 48);

                // Luminous cyan progress trace along bottom edge
                int barWidth = (w * g_SplashProgress) / 100;
                Gdiplus::SolidBrush barBrush(Gdiplus::Color(255, 0, 200, 255));
                graphics.FillRectangle(&barBrush, 0, h - 3, barWidth, 3);

                // Status message
                Gdiplus::FontFamily fontFamily(L"Segoe UI");
                Gdiplus::Font font(&fontFamily, 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush textBrush(Gdiplus::Color(240, 225, 235, 245));

                std::wstring statusMsg = L"Initializing TitanZFS...";
                if (g_SplashProgress > 40) statusMsg = L"Verifying system prerequisites and storage topology...";
                if (g_SplashProgress > 80) statusMsg = L"Loading TitanZFS Setup Wizard...";

                Gdiplus::PointF origin(18.0f, static_cast<Gdiplus::REAL>(h - 35));
                graphics.DrawString(statusMsg.c_str(), -1, &font, origin, &textBrush);
            }

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_KEYDOWN: {
            KillTimer(hwnd, 1);
            DestroyWindow(hwnd);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    g_hInstance = hInstance;

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    std::wstring cmdLine(pCmdLine ? pCmdLine : L"");
    std::transform(cmdLine.begin(), cmdLine.end(), cmdLine.begin(), ::towlower);

    bool isUninstall = (cmdLine.find(L"/uninstall") != std::wstring::npos || cmdLine.find(L"-uninstall") != std::wstring::npos);
    bool isSilent = (cmdLine.find(L"/silent") != std::wstring::npos || cmdLine.find(L"/s") != std::wstring::npos || cmdLine.find(L"/quiet") != std::wstring::npos || cmdLine.find(L"/qn") != std::wstring::npos);

    if (isUninstall) {
        bool res = PerformUninstall(isSilent);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return res ? 0 : 1;
    }

    if (isSilent) {
        std::wstring dstDir = L"C:\\Program Files\\TitanZFS";
        bool res = PerformInstall(dstDir, true, true, true, true, true, nullptr, nullptr);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return res ? 0 : 1;
    }

    g_HeroImagePath = FindHeroImage();

    // 1. Show Cinematic Hero Splash Screen
    const wchar_t SPLASH_CLASS[] = L"TitanZFS_Splash_Class";
    WNDCLASSEXW splashWc{};
    splashWc.cbSize = sizeof(splashWc);
    splashWc.style = CS_HREDRAW | CS_VREDRAW;
    splashWc.lpfnWndProc = SplashWndProc;
    splashWc.hInstance = hInstance;
    splashWc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    splashWc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    splashWc.lpszClassName = SPLASH_CLASS;
    RegisterClassExW(&splashWc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int splashW = 640;
    int splashH = 360;
    int splashX = (screenW - splashW) / 2;
    int splashY = (screenH - splashH) / 2;

    HWND hSplash = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        SPLASH_CLASS,
        L"TitanZFS Splash",
        WS_POPUP,
        splashX, splashY, splashW, splashH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (hSplash) {
        ShowWindow(hSplash, SW_SHOW);
        UpdateWindow(hSplash);

        MSG splashMsg{};
        while (IsWindow(hSplash) && GetMessageW(&splashMsg, nullptr, 0, 0)) {
            TranslateMessage(&splashMsg);
            DispatchMessageW(&splashMsg);
        }
    }

    // 2. Initialize Main Setup Wizard (Spacious Left-Sidebar Studio Layout)
    INITCOMMONCONTROLSEX icex{
        .dwSize = sizeof(icex),
        .dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES
    };
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"TitanZFS_Installer_Class";

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
        L"TitanZFS Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 830, 560,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hMainWnd) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);
    SetForegroundWindow(g_hMainWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(msg.wParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    return wWinMain(hInstance, nullptr, GetCommandLineW(), nCmdShow);
}
