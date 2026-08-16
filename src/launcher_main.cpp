#include <Windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "bindings.h"
#include "controller_ipc.h"

namespace fs = std::filesystem;

namespace {
HWND g_window = nullptr;
fs::path g_installRoot;
std::wstring g_status = L"Starting SteamVR...";
bool g_viewerEnabled = false;
bool g_bindingMenu = false;
int g_captureAction = -1;
std::map<bigscreen_bindings::Action, bigscreen_bindings::Binding> g_bindings;
bigscreen_desktop_controller_ipc::ControllerState g_lastController{};
HANDLE g_controllerMapping = nullptr;
const bigscreen_bindings::Action g_bindingRows[] = {
    bigscreen_bindings::Action::MoveForward, bigscreen_bindings::Action::MoveBackward,
    bigscreen_bindings::Action::MoveLeft, bigscreen_bindings::Action::MoveRight,
    bigscreen_bindings::Action::LookToggle, bigscreen_bindings::Action::Reset,
    bigscreen_bindings::Action::VRTrigger, bigscreen_bindings::Action::VRGrip,
    bigscreen_bindings::Action::VRMenu, bigscreen_bindings::Action::VRTrackpadClick,
    bigscreen_bindings::Action::MicToggle, bigscreen_bindings::Action::ArmUp,
    bigscreen_bindings::Action::ArmDown, bigscreen_bindings::Action::ArmLeft,
    bigscreen_bindings::Action::ArmRight,
};
int g_waitSeconds = 0;
RECT g_viewerButton{48, 500, 430, 580};
RECT g_minButton{890, 16, 925, 50};
RECT g_closeButton{932, 12, 970, 52};
RECT g_bindButton{650, 14, 860, 54};
RECT g_bindingCloseButton{870, 22, 945, 58};

int ActionIndex(bigscreen_bindings::Action action) {
    for (int i = 0; i < static_cast<int>(std::size(g_bindingRows)); ++i) if (g_bindingRows[i] == action) return i;
    return -1;
}

void SaveBindings() { bigscreen_bindings::Save(bigscreen_bindings::SettingsPath(), g_bindings); }

void ResetBindings() { g_bindings = bigscreen_bindings::Defaults(); SaveBindings(); }

bool ReadController(bigscreen_desktop_controller_ipc::ControllerState& out) {
    if (!g_controllerMapping) {
        g_controllerMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_desktop_controller_ipc::kMappingName);
        if (!g_controllerMapping) return false;
    }
    auto* state = static_cast<const bigscreen_desktop_controller_ipc::ControllerState*>(
        MapViewOfFile(g_controllerMapping, FILE_MAP_READ, 0, 0, sizeof(out)));
    if (!state) return false;
    const bool ok = bigscreen_desktop_controller_ipc::Read(state, out);
    UnmapViewOfFile(state);
    return ok;
}

void PollGamepadCapture() {
    if (g_captureAction < 0) return;
    bigscreen_desktop_controller_ipc::ControllerState current{};
    if (!ReadController(current)) return;
    const uint32_t newButtons = current.buttons & ~g_lastController.buttons;
    const uint32_t newDpad = current.dpad != g_lastController.dpad ? current.dpad : 0;
    int code = -1;
    if (newButtons) for (int bit = 0; bit < 10; ++bit) if (newButtons & (1u << bit)) { code = bit; break; }
    if (code < 0 && newDpad && newDpad != 8) code = static_cast<int>(newDpad);
    g_lastController = current;
    if (code < 0) return;
    const auto action = g_bindingRows[g_captureAction];
    g_bindings[action] = {bigscreen_bindings::Device::Gamepad, code};
    SaveBindings();
    g_captureAction = -1;
    InvalidateRect(g_window, nullptr, FALSE);
}

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
    RoundBox(dc, g_bindButton, RGB(5, 37, 66), RGB(22, 128, 208), 10);
    DrawTextAt(dc, L"BIND CONTROLS", g_bindButton.left, 17, g_bindButton.right - g_bindButton.left,
               32, 13, FW_BOLD, RGB(225, 242, 255), DT_CENTER | DT_VCENTER);
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

    if (g_bindingMenu) {
        HBRUSH overlay = CreateSolidBrush(RGB(2, 10, 21));
        FillRect(dc, &client, overlay); DeleteObject(overlay);
        RoundBox(dc, RECT{28, 20, client.right - 28, client.bottom - 20}, RGB(4, 20, 38), RGB(25, 123, 190), 22);
        DrawTextAt(dc, L"BIND CONTROLS", 60, 38, 500, 42, 30, FW_BOLD, RGB(240, 248, 255), DT_LEFT | DT_VCENTER);
        DrawTextAt(dc, L"Click a binding, then press a key, mouse button, or gamepad control.",
                   62, 82, 700, 28, 14, FW_NORMAL, RGB(165, 194, 220), DT_LEFT | DT_VCENTER);
        DrawTextAt(dc, L"\u00D7", g_bindingCloseButton.left, 18, 70, 42, 30, FW_NORMAL, RGB(160, 178, 200), DT_CENTER | DT_VCENTER);
        const int rowTop = 125;
        for (int i = 0; i < static_cast<int>(std::size(g_bindingRows)); ++i) {
            const int y = rowTop + i * 27;
            const bool waiting = g_captureAction == i;
            if (waiting) RoundBox(dc, RECT{54, y - 2, client.right - 54, y + 24}, RGB(11, 76, 116), RGB(44, 200, 255), 6);
            DrawTextAt(dc, bigscreen_bindings::Name(g_bindingRows[i]), 70, y, 300, 23, 14, FW_NORMAL,
                       RGB(230, 240, 249), DT_LEFT | DT_VCENTER);
            const auto it = g_bindings.find(g_bindingRows[i]);
            const std::wstring label = waiting ? L"Waiting for input..." : bigscreen_bindings::BindingLabel(it->second);
            DrawTextAt(dc, label.c_str(), 430, y, 380, 23, 14, waiting ? FW_BOLD : FW_NORMAL,
                       waiting ? RGB(80, 220, 255) : RGB(190, 216, 235), DT_LEFT | DT_VCENTER);
        }
        RoundBox(dc, RECT{54, 555, 250, 595}, RGB(6, 44, 72), RGB(24, 123, 182), 8);
        DrawTextAt(dc, L"RESET TO DEFAULTS", 54, 558, 196, 34, 12, FW_BOLD, RGB(220, 238, 250), DT_CENTER | DT_VCENTER);
        DrawTextAt(dc, L"ESC cancels capture", 650, 562, 250, 24, 13, FW_NORMAL, RGB(160, 194, 220), DT_RIGHT | DT_VCENTER);
    }
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
    if (message == WM_TIMER && wParam == 2) { PollGamepadCapture(); return 0; }
    if (message == WM_KEYDOWN && g_bindingMenu && g_captureAction >= 0) {
        if (wParam == VK_ESCAPE) { g_captureAction = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        const auto action = g_bindingRows[g_captureAction];
        g_bindings[action] = {bigscreen_bindings::Device::Keyboard, static_cast<int>(wParam)};
        SaveBindings(); g_captureAction = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    if (message == WM_LBUTTONDOWN && !g_bindingMenu) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (PtInRect(&g_bindButton, point)) {
            g_bindingMenu = true;
            g_captureAction = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
    }
    if (message == WM_LBUTTONDOWN && g_bindingMenu && g_captureAction >= 0) {
        const auto action = g_bindingRows[g_captureAction];
        g_bindings[action] = {bigscreen_bindings::Device::Mouse, 1};
        SaveBindings(); g_captureAction = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    if (message == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!g_bindingMenu && PtInRect(&g_bindButton, point)) {
            g_bindingMenu = true; g_captureAction = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        if (g_bindingMenu) {
            if (PtInRect(&g_bindingCloseButton, point)) { g_bindingMenu = false; g_captureAction = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            const RECT resetRect{54, 555, 250, 595};
            if (PtInRect(&resetRect, point)) { ResetBindings(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            const int rowTop = 125;
            for (int i = 0; i < static_cast<int>(std::size(g_bindingRows)); ++i) {
                RECT row{54, rowTop + i * 27 - 2, 926, rowTop + i * 27 + 24};
                if (PtInRect(&row, point)) { g_captureAction = i; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            }
            return 0;
        }
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
            if (point.y < 72 && !PtInRect(&g_closeButton, point) && !PtInRect(&g_minButton, point) &&
                !PtInRect(&g_bindButton, point)) return HTCAPTION;
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
    g_bindings = bigscreen_bindings::Load(bigscreen_bindings::SettingsPath());
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Bigscreen Desktop",
                               WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 980, 620,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    SetWindowRgn(g_window, CreateRoundRectRgn(0, 0, 980, 620, 30, 30), TRUE);
    ShowWindow(g_window, show); UpdateWindow(g_window); SetTimer(g_window, 2, 50, nullptr); StartSequence();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (g_controllerMapping) CloseHandle(g_controllerMapping);
    return 0;
}
