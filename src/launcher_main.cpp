#include <Windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
HWND g_window = nullptr;
fs::path g_installRoot;
std::wstring g_status = L"Starting SteamVR...";
bool g_viewerEnabled = false;
int g_waitSeconds = 0;
RECT g_viewerButton{48, 500, 430, 580};
RECT g_minButton{890, 16, 925, 50};
RECT g_closeButton{932, 12, 970, 52};

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

void SetStatus(const wchar_t* text) {
    g_status = text;
    InvalidateRect(g_window, nullptr, FALSE);
    UpdateWindow(g_window);
}

void UseLauncherFont(HDC dc, int size, int weight, COLORREF color, HFONT& old) {
    HFONT font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    old = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
}

void DrawTextAt(HDC dc, const wchar_t* text, int x, int y, int w, int h,
                int size, int weight, COLORREF color, UINT format) {
    HFONT old{};
    UseLauncherFont(dc, size, weight, color, old);
    RECT rect{x, y, x + w, y + h};
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &rect, format);
    HFONT current = static_cast<HFONT>(SelectObject(dc, old));
    DeleteObject(current);
}

void RoundBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void Paint(HDC dc) {
    RECT client{};
    GetClientRect(g_window, &client);
    HBRUSH background = CreateSolidBrush(RGB(3, 13, 27));
    FillRect(dc, &client, background);
    DeleteObject(background);

    RoundBox(dc, RECT{1, 1, client.right - 1, client.bottom - 1}, RGB(4, 17, 33), RGB(18, 61, 96), 28);
    HPEN line = CreatePen(PS_SOLID, 1, RGB(19, 54, 82));
    HGDIOBJ old = SelectObject(dc, line);
    MoveToEx(dc, 1, 72, nullptr); LineTo(dc, client.right - 1, 72);
    MoveToEx(dc, 1, 460, nullptr); LineTo(dc, client.right - 1, 460);
    SelectObject(dc, old); DeleteObject(line);

    RoundBox(dc, RECT{24, 20, 58, 54}, RGB(18, 126, 224), RGB(60, 190, 255), 5);
    DrawTextAt(dc, L"B", 24, 18, 34, 38, 25, FW_BOLD, RGB(240, 250, 255), DT_CENTER | DT_VCENTER);
    DrawTextAt(dc, L"Bigscreen Desktop", 76, 18, 420, 38, 24, FW_NORMAL, RGB(232, 239, 250), DT_LEFT | DT_VCENTER);
    DrawTextAt(dc, L"\u2014", g_minButton.left, 12, 35, 35, 22, FW_NORMAL, RGB(160, 178, 200), DT_CENTER | DT_VCENTER);
    DrawTextAt(dc, L"\u00D7", g_closeButton.left, 8, 38, 40, 30, FW_NORMAL, RGB(160, 178, 200), DT_CENTER | DT_VCENTER);

    DrawTextAt(dc, L"BIGSCREEN", 0, 102, client.right, 65, 52, FW_BOLD, RGB(244, 247, 253), DT_CENTER | DT_VCENTER);
    DrawTextAt(dc, L"DESKTOP BRIDGE", 0, 166, client.right, 42, 25, FW_BOLD, RGB(35, 175, 255), DT_CENTER | DT_VCENTER);

    RoundBox(dc, RECT{45, 255, client.right - 45, 420}, RGB(5, 25, 46), RGB(18, 80, 132), 20);
    RoundBox(dc, RECT{73, 286, 155, 368}, RGB(5, 42, 75), RGB(22, 128, 208), 42);
    HPEN iconPen = CreatePen(PS_SOLID, 3, RGB(31, 174, 255));
    HGDIOBJ oldPen = SelectObject(dc, iconPen);
    Rectangle(dc, 94, 308, 136, 338);
    MoveToEx(dc, 115, 338, nullptr); LineTo(dc, 115, 349); MoveToEx(dc, 102, 352, nullptr); LineTo(dc, 128, 352);
    SelectObject(dc, oldPen); DeleteObject(iconPen);
    DrawTextAt(dc, g_status.c_str(), 185, 292, client.right - 230, 100, 22, FW_NORMAL,
               RGB(229, 239, 250), DT_LEFT | DT_WORDBREAK | DT_VCENTER);

    const COLORREF buttonFill = g_viewerEnabled ? RGB(17, 104, 210) : RGB(28, 58, 84);
    const COLORREF buttonBorder = g_viewerEnabled ? RGB(42, 205, 255) : RGB(45, 80, 106);
    RoundBox(dc, g_viewerButton, buttonFill, buttonBorder, 18);
    DrawTextAt(dc, L"\u25B6", 76, 515, 48, 48, 27, FW_BOLD, RGB(245, 250, 255), DT_CENTER | DT_VCENTER);
    DrawTextAt(dc, L"OPEN VIEWER", 132, 514, 265, 50, 22, FW_BOLD,
               g_viewerEnabled ? RGB(255, 255, 255) : RGB(160, 180, 198), DT_CENTER | DT_VCENTER);

    RoundBox(dc, RECT{462, 500, client.right - 45, 580}, RGB(5, 25, 46), RGB(18, 80, 132), 18);
    DrawTextAt(dc, g_viewerEnabled ? L"\u2713" : L"\u2022", 484, 515, 45, 45, 30, FW_BOLD,
               g_viewerEnabled ? RGB(50, 220, 140) : RGB(80, 150, 190), DT_CENTER | DT_VCENTER);
    DrawTextAt(dc, g_viewerEnabled ? L"When ready, start Bigscreen normally." : L"Waiting for SteamVR...",
               540, 516, client.right - 600, 40, 16, FW_NORMAL, RGB(222, 234, 247), DT_LEFT | DT_VCENTER);
}

void StartSequence() {
    const fs::path steam = FindSteam();
    if (steam.empty()) { SetStatus(L"Steam was not found."); return; }
    const fs::path vrmonitor = steam / L"steamapps\\common\\SteamVR\\bin\\win64\\vrmonitor.exe";
    if (!fs::exists(vrmonitor)) { SetStatus(L"SteamVR was not found."); return; }
    if (!ProcessExists(L"vrmonitor.exe")) StartVisible(vrmonitor);
    SetStatus(L"Starting SteamVR...");
    SetTimer(g_window, 1, 1000, nullptr);
}

void CheckReady() {
    ++g_waitSeconds;
    if (!ProcessExists(L"vrserver.exe") || !ProcessExists(L"vrcompositor.exe")) {
        if (g_waitSeconds >= 45) { KillTimer(g_window, 1); SetStatus(L"SteamVR did not become ready. Check SteamVR and restart this launcher."); }
        return;
    }
    KillTimer(g_window, 1);
    const fs::path bridge = g_installRoot / L"BigscreenDesktopBridge.exe";
    if (!ProcessExists(L"BigscreenDesktopBridge.exe") && !StartHidden(bridge)) { SetStatus(L"Could not start the bridge."); return; }
    g_viewerEnabled = true;
    SetStatus(L"Ready. SteamVR and the bridge are running in the background. Start Bigscreen, then open the viewer.");
    InvalidateRect(g_window, nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) { PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); Paint(dc); EndPaint(hwnd, &ps); return 0; }
    if (message == WM_TIMER && wParam == 1) { CheckReady(); return 0; }
    if (message == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (PtInRect(&g_closeButton, point)) { DestroyWindow(hwnd); return 0; }
        if (PtInRect(&g_minButton, point)) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
        if (g_viewerEnabled && PtInRect(&g_viewerButton, point)) {
            const fs::path viewer = g_installRoot / L"BigscreenDesktopViewer.exe";
            if (ProcessExists(L"BigscreenDesktopViewer.exe") || StartHidden(viewer)) SetStatus(L"Viewer opened. SteamVR and the bridge remain running in the background.");
            else SetStatus(L"Could not open the viewer.");
            return 0;
        }
    }
    if (message == WM_NCHITTEST) {
        LRESULT hit = DefWindowProcW(hwnd, message, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            if (point.y < 72 && !PtInRect(&g_closeButton, point) && !PtInRect(&g_minButton, point)) return HTCAPTION;
        }
        return hit;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_installRoot = ModuleDir();
    WNDCLASSW wc{};
    wc.hInstance = instance; wc.lpfnWndProc = WindowProc; wc.lpszClassName = L"BigscreenDesktopLauncher";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Bigscreen Desktop",
                               WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 980, 620,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    SetWindowRgn(g_window, CreateRoundRectRgn(0, 0, 980, 620, 30, 30), TRUE);
    ShowWindow(g_window, show); UpdateWindow(g_window); StartSequence();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
