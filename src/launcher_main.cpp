#include <Windows.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
HWND g_window = nullptr;
HWND g_status = nullptr;
HWND g_viewerButton = nullptr;
fs::path g_installRoot;
int g_waitSeconds = 0;

bool ProcessExists(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) { found = true; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool StartVisible(const fs::path& path) {
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr,
                                     path.parent_path().c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool StartHidden(const fs::path& path) {
    std::wstring command = L"\"" + path.wstring() + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const bool ok = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, path.parent_path().c_str(),
                                   &startup, &process) != FALSE;
    if (ok) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    return ok;
}

fs::path ModuleDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

fs::path FindSteam() {
    const wchar_t* p86 = _wgetenv(L"ProgramFiles(x86)");
    const wchar_t* p64 = _wgetenv(L"ProgramFiles");
    if (p86 && fs::exists(fs::path(p86) / L"Steam")) return fs::path(p86) / L"Steam";
    if (p64 && fs::exists(fs::path(p64) / L"Steam")) return fs::path(p64) / L"Steam";
    if (fs::exists(L"C:\\Steam")) return L"C:\\Steam";
    return {};
}

void Status(const wchar_t* text) { SetWindowTextW(g_status, text); }

void StartSequence() {
    const fs::path steam = FindSteam();
    if (steam.empty()) { Status(L"Steam was not found."); return; }
    const fs::path vrmonitor = steam / L"steamapps\\common\\SteamVR\\bin\\win64\\vrmonitor.exe";
    if (!fs::exists(vrmonitor)) { Status(L"SteamVR was not found."); return; }
    if (!ProcessExists(L"vrmonitor.exe")) StartVisible(vrmonitor);
    Status(L"Starting SteamVR...");
    SetTimer(g_window, 1, 1000, nullptr);
}

void CheckReady() {
    ++g_waitSeconds;
    if (!ProcessExists(L"vrserver.exe") || !ProcessExists(L"vrcompositor.exe")) {
        if (g_waitSeconds >= 45) {
            KillTimer(g_window, 1);
            Status(L"SteamVR did not become ready. Check SteamVR and restart this launcher.");
        }
        return;
    }
    KillTimer(g_window, 1);
    const fs::path bridge = g_installRoot / L"BigscreenDesktopBridge.exe";
    if (!ProcessExists(L"BigscreenDesktopBridge.exe") && !StartHidden(bridge)) {
        Status(L"Could not start the bridge.");
        return;
    }
    Status(L"Ready. SteamVR and the bridge are running in the background. Start Bigscreen, then open the viewer.");
    EnableWindow(g_viewerButton, TRUE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == 1) { CheckReady(); return 0; }
    if (message == WM_COMMAND && LOWORD(wParam) == 1001) {
        const fs::path viewer = g_installRoot / L"BigscreenDesktopViewer.exe";
        if (ProcessExists(L"BigscreenDesktopViewer.exe") || StartVisible(viewer))
            Status(L"Viewer opened. SteamVR and the bridge remain running in the background.");
        else Status(L"Could not open the viewer.");
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_installRoot = ModuleDir();
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"BigscreenDesktopLauncher";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Bigscreen Desktop",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, 610, 230,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    CreateWindowW(L"STATIC", L"Bigscreen Desktop Bridge", WS_CHILD | WS_VISIBLE,
                  24, 20, 540, 32, g_window, nullptr, instance, nullptr);
    g_status = CreateWindowW(L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE,
                             24, 70, 540, 58, g_window, nullptr, instance, nullptr);
    g_viewerButton = CreateWindowW(L"BUTTON", L"Open Viewer", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                   24, 145, 170, 34, g_window, reinterpret_cast<HMENU>(1001), instance, nullptr);
    CreateWindowW(L"STATIC", L"When ready, start Bigscreen normally.", WS_CHILD | WS_VISIBLE,
                  220, 153, 330, 24, g_window, nullptr, instance, nullptr);
    ShowWindow(g_window, show);
    UpdateWindow(g_window);
    StartSequence();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
