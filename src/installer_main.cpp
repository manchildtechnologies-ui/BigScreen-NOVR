#include <Windows.h>
#include <objidl.h>
#include <shellapi.h>
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define BIGSCREEN_INSTALLER_MINMAX_DEFINED
#endif
#include <gdiplus.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef BIGSCREEN_INSTALLER_MINMAX_DEFINED
#undef min
#undef max
#undef BIGSCREEN_INSTALLER_MINMAX_DEFINED
#endif

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;
using namespace Gdiplus;

namespace {
HWND g_window = nullptr;
std::wstring g_status = L"Preparing installation...";
ULONG_PTR g_gdiplusToken = 0;
std::unique_ptr<Image> g_image;

fs::path ModuleDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

int Fail(const std::wstring& text) {
    MessageBoxW(g_window, text.c_str(), L"Bigscreen Desktop Installer", MB_OK | MB_ICONERROR);
    return 1;
}

void AddSteamCandidate(std::vector<fs::path>& candidates, const fs::path& path) {
    if (path.empty() || !fs::exists(path)) return;
    std::error_code error;
    const fs::path normalized = fs::weakly_canonical(path, error);
    const fs::path value = error ? path : normalized;
    for (const auto& candidate : candidates) if (candidate == value) return;
    candidates.push_back(value);
}

void AddRegistrySteamCandidate(std::vector<fs::path>& candidates, HKEY root, const wchar_t* subkey,
                               REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return;
    wchar_t value[MAX_PATH * 4]{};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, L"InstallPath", nullptr, &type,
                         reinterpret_cast<LPBYTE>(value), &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        AddSteamCandidate(candidates, value);
    }
    RegCloseKey(key);
}

void AddLibraryFolderCandidates(std::vector<fs::path>& candidates, const fs::path& steam) {
    const fs::path file = steam / L"steamapps\\libraryfolders.vdf";
    std::ifstream input(file);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        const std::string key = "\"path\"";
        const size_t keyPos = line.find(key);
        if (keyPos == std::string::npos) continue;
        const size_t firstQuote = line.find('"', keyPos + key.size());
        if (firstQuote == std::string::npos) continue;
        const size_t secondQuote = line.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos) continue;
        std::string value = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        for (size_t pos = 0; (pos = value.find("\\\\", pos)) != std::string::npos; ) value.replace(pos, 2, "\\");
        AddSteamCandidate(candidates, fs::path(std::wstring(value.begin(), value.end())));
    }
}

bool HasSteamVr(const fs::path& steam) {
    return !steam.empty() && fs::exists(steam / L"steamapps\\common\\SteamVR\\bin\\win64\\vrmonitor.exe") &&
           fs::exists(steam / L"steamapps\\common\\SteamVR\\drivers");
}

fs::path FindSteam() {
    std::vector<fs::path> candidates;
    const wchar_t* p86 = _wgetenv(L"ProgramFiles(x86)");
    const wchar_t* p64 = _wgetenv(L"ProgramFiles");
    if (p86) AddSteamCandidate(candidates, fs::path(p86) / L"Steam");
    if (p64) AddSteamCandidate(candidates, fs::path(p64) / L"Steam");
    AddSteamCandidate(candidates, L"C:\\Steam");
    AddRegistrySteamCandidate(candidates, HKEY_CURRENT_USER, L"Software\\Valve\\Steam", KEY_WOW64_64KEY);
    AddRegistrySteamCandidate(candidates, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", KEY_WOW64_64KEY);
    AddRegistrySteamCandidate(candidates, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", KEY_WOW64_32KEY);
    for (size_t i = 0; i < candidates.size(); ++i) AddLibraryFolderCandidates(candidates, candidates[i]);
    for (const auto& candidate : candidates) if (HasSteamVr(candidate)) return candidate;
    return {};
}

fs::path PickSteamFolder() {
    BROWSEINFOW browse{};
    browse.hwndOwner = g_window;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    browse.lpszTitle = L"SteamVR was not found automatically. Select your Steam installation folder.";
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&browse);
    if (!selected) return {};
    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(selected, path) != FALSE;
    CoTaskMemFree(selected);
    return ok ? fs::path(path) : fs::path{};
}

void SetStatus(const wchar_t* text) {
    g_status = text;
    InvalidateRect(g_window, nullptr, FALSE);
    UpdateWindow(g_window);
}

bool CreateShortcut(const fs::path& target, const fs::path& iconPath, const wchar_t* shortcutName) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(target.parent_path().c_str());
    link->SetIconLocation(iconPath.c_str(), 0);
    link->SetDescription(L"Start Bigscreen Desktop");
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)));
    if (ok) {
        wchar_t desktop[MAX_PATH]{};
        SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktop);
        const fs::path shortcut = fs::path(desktop) / shortcutName;
        ok = SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

bool InstallPayload() {
    const fs::path package = ModuleDir();
    fs::path steam = FindSteam();
    if (!HasSteamVr(steam)) steam = PickSteamFolder();
    if (!HasSteamVr(steam)) return false;
    const fs::path steamVr = steam / L"steamapps\\common\\SteamVR";
    const fs::path driverSource = package / L"steamvr_driver\\driver_bigscreen_desktop";
    const fs::path driverTarget = steamVr / L"drivers\\driver_bigscreen_desktop";
    if (!fs::exists(steamVr / L"drivers") || !fs::exists(driverSource / L"driver.vrdrivermanifest")) return false;

    SetStatus(L"Installing Bigscreen Desktop files...");
    const fs::path install = fs::path(_wgetenv(L"LOCALAPPDATA")) / L"BigscreenDesktopBridge";
    fs::create_directories(install);
    std::wofstream selectedSteam(install / L"steam_path.txt", std::ios::trunc);
    if (selectedSteam) selectedSteam << steam.wstring();
    fs::copy_file(package / L"BigscreenDesktopBridge.exe", install / L"BigscreenDesktopBridge.exe", fs::copy_options::overwrite_existing);
    fs::copy_file(package / L"BigscreenDesktopViewer.exe", install / L"BigscreenDesktopViewer.exe", fs::copy_options::overwrite_existing);
    fs::copy_file(package / L"BigscreenDesktopLauncher.exe", install / L"BigscreenDesktopLauncher.exe", fs::copy_options::overwrite_existing);
    fs::copy_file(package / L"BigscreenDesktopUninstaller.exe", install / L"BigscreenDesktopUninstaller.exe", fs::copy_options::overwrite_existing);
    fs::copy_file(package / L"Bigscreen.ico", install / L"Bigscreen.ico", fs::copy_options::overwrite_existing);

    SetStatus(L"Installing SteamVR controller driver...");
    fs::create_directories(driverTarget);
    fs::copy(driverSource, driverTarget, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    SetStatus(L"Creating desktop shortcut...");
    if (!CreateShortcut(install / L"BigscreenDesktopLauncher.exe", install / L"Bigscreen.ico", L"Start Bigscreen Desktop.lnk")) return false;
    if (!CreateShortcut(install / L"BigscreenDesktopUninstaller.exe", install / L"Bigscreen.ico", L"Uninstall Bigscreen Desktop Bridge.lnk")) return false;
    SetStatus(L"Installation complete.");
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (g_image) {
            Graphics graphics(dc);
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            const REAL scale = std::min(static_cast<REAL>(client.right) / g_image->GetWidth(),
                                        static_cast<REAL>(client.bottom - 44) / g_image->GetHeight());
            const int width = static_cast<int>(g_image->GetWidth() * scale);
            const int height = static_cast<int>(g_image->GetHeight() * scale);
            graphics.DrawImage(g_image.get(), (client.right - width) / 2, 0, width, height);
        }
        RECT status{20, client.bottom - 40, client.right - 20, client.bottom - 10};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, g_status.c_str(), -1, &status, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_APP + 1) {
        const bool ok = InstallPayload();
        if (ok) {
            MessageBoxW(hwnd, L"Installation complete.\n\nSteamVR, the bridge, and the viewer are installed.\n\nNow start Bigscreen.",
                        L"Bigscreen Desktop Ready", MB_OK | MB_ICONINFORMATION);
            DestroyWindow(hwnd);
        } else {
            Fail(L"Installation could not be completed. Make sure SteamVR is installed and run this installer as administrator.");
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    if (!IsUserAnAdmin()) {
        const fs::path self = ModuleDir();
        SHELLEXECUTEINFOW exec{sizeof(exec)};
        exec.lpVerb = L"runas";
        exec.lpFile = self.c_str();
        exec.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&exec)) return 1;
        return 0;
    }
    GdiplusStartupInput startup{};
    GdiplusStartup(&g_gdiplusToken, &startup, nullptr);
    const fs::path imagePath = ModuleDir() / L"BigscreenInstaller.png";
    g_image = std::make_unique<Image>(imagePath.c_str());

    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"BigscreenDesktopInstaller";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Bigscreen Desktop Installer",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, 960, 620,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    ShowWindow(g_window, show);
    UpdateWindow(g_window);
    PostMessageW(g_window, WM_APP + 1, 0, 0);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_image.reset();
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
