#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

namespace {
constexpr COLORREF kBg = RGB(5, 12, 24);
constexpr COLORREF kPanel = RGB(12, 22, 36);
constexpr COLORREF kPanel2 = RGB(17, 29, 45);
constexpr COLORREF kBorder = RGB(46, 67, 91);
constexpr COLORREF kText = RGB(241, 245, 252);
constexpr COLORREF kMuted = RGB(163, 181, 207);
constexpr COLORREF kAccent = RGB(42, 125, 255);
constexpr COLORREF kAccentBright = RGB(76, 169, 255);
constexpr COLORREF kGreen = RGB(91, 205, 137);
constexpr COLORREF kAmber = RGB(235, 181, 77);

HWND g_window = nullptr;
bool g_settingsOpen = false;
bool g_launchSteamVR = true;
bool g_launchBigscreen = false;
bool g_keepViewerTop = true;
bool g_playing = false;
bool g_thirdPerson = false;
int g_clickedButton = 0;
int g_clickTicks = 0;
std::wstring g_status = L"Ready to play";
std::wstring g_lastError;
UINT_PTR g_timer = 0;

RECT g_playRect{}, g_firstRect{}, g_thirdRect{}, g_settingsRect{}, g_settingsCloseRect{};
RECT g_autoSteamRect{}, g_autoBigscreenRect{}, g_keepTopRect{};

fs::path ModuleDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

fs::path SettingsPath() {
    wchar_t* raw = nullptr;
    size_t length = 0;
    _wdupenv_s(&raw, &length, L"LOCALAPPDATA");
    fs::path root = raw ? fs::path(raw) / L"BigscreenNOVR" : ModuleDir();
    if (raw) free(raw);
    std::error_code ec;
    fs::create_directories(root, ec);
    return root / L"settings.ini";
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

bool StartProcess(const fs::path& path, bool visible) {
    if (!fs::exists(path)) return false;
    std::wstring command = L"\"" + path.wstring() + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    DWORD flags = visible ? 0 : CREATE_NO_WINDOW;
    bool ok = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, flags, nullptr,
                             path.parent_path().c_str(), &startup, &process) != FALSE;
    if (ok) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    return ok;
}

fs::path FindSteam() {
    const wchar_t* p86 = _wgetenv(L"ProgramFiles(x86)");
    const wchar_t* p64 = _wgetenv(L"ProgramFiles");
    if (p86 && fs::exists(fs::path(p86) / L"Steam")) return fs::path(p86) / L"Steam";
    if (p64 && fs::exists(fs::path(p64) / L"Steam")) return fs::path(p64) / L"Steam";
    if (fs::exists(L"C:\\Steam")) return L"C:\\Steam";
    return {};
}

fs::path FindBeside(const wchar_t* name) {
    fs::path path = ModuleDir() / name;
    return fs::exists(path) ? path : fs::path{};
}

HWND ViewerWindow() { return FindWindowA("BigscreenDesktopViewer", nullptr); }

void ApplyView(bool third) {
    g_thirdPerson = third;
    HWND viewer = ViewerWindow();
    if (viewer) {
        PostMessageW(viewer, WM_KEYDOWN, third ? '3' : '1', 0);
        PostMessageW(viewer, WM_KEYUP, third ? '3' : '1', 0);
        if (g_keepViewerTop) SetWindowPos(viewer, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    InvalidateRect(g_window, nullptr, FALSE);
}

void SaveSettings() {
    std::wofstream out(SettingsPath(), std::ios::trunc);
    if (!out) return;
    out << L"launch_steamvr=" << (g_launchSteamVR ? 1 : 0) << L"\n";
    out << L"launch_bigscreen=" << (g_launchBigscreen ? 1 : 0) << L"\n";
    out << L"keep_viewer_top=" << (g_keepViewerTop ? 1 : 0) << L"\n";
    out << L"start_view=" << (g_thirdPerson ? 3 : 1) << L"\n";
}

void LoadSettings() {
    std::wifstream in(SettingsPath());
    std::wstring line;
    while (std::getline(in, line)) {
        const auto split = line.find(L'=');
        if (split == std::wstring::npos) continue;
        const auto key = line.substr(0, split);
        const bool value = line.substr(split + 1) == L"1";
        if (key == L"launch_steamvr") g_launchSteamVR = value;
        else if (key == L"launch_bigscreen") g_launchBigscreen = value;
        else if (key == L"keep_viewer_top") g_keepViewerTop = value;
        else if (key == L"start_view") g_thirdPerson = line.substr(split + 1) == L"3";
    }
}

void SetStatus(const std::wstring& text) { g_status = text; InvalidateRect(g_window, nullptr, FALSE); }

void Play() {
    g_playing = true;
    bool startedSomething = false;
    if (g_launchSteamVR && !ProcessExists(L"vrmonitor.exe")) {
        const fs::path steam = FindSteam();
        const fs::path vr = steam.empty() ? fs::path{} : steam / L"steamapps\\common\\SteamVR\\bin\\win64\\vrmonitor.exe";
        if (!vr.empty() && StartProcess(vr, true)) startedSomething = true;
    }
    const fs::path bridge = FindBeside(L"BigscreenDesktopBridge.exe");
    if (!ProcessExists(L"BigscreenDesktopBridge.exe") && !bridge.empty() && StartProcess(bridge, false)) startedSomething = true;
    const fs::path viewer = FindBeside(L"BigscreenDesktopViewer.exe");
    if (!ProcessExists(L"BigscreenDesktopViewer.exe") && !viewer.empty() && StartProcess(viewer, true)) startedSomething = true;
    if (g_launchBigscreen) {
        const fs::path game = FindBeside(L"Bigscreen.exe");
        if (!game.empty() && !ProcessExists(L"Bigscreen.exe")) startedSomething |= StartProcess(game, true);
    }
    SetStatus(startedSomething ? L"NOVR is starting" : L"Ready - start Bigscreen to begin");
    SetTimer(g_window, 1, 750, nullptr);
    ApplyView(g_thirdPerson);
}

void Stop() {
    g_playing = false;
    SetStatus(L"Ready to play");
    KillTimer(g_window, 1);
    InvalidateRect(g_window, nullptr, FALSE);
}

COLORREF StatusColor(bool on, bool waiting = false) { return on ? kGreen : (waiting ? kAmber : kMuted); }

void Font(HDC dc, int size, int weight, COLORREF color, HFONT& old) {
    HFONT f = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    old = static_cast<HFONT>(SelectObject(dc, f));
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
}

void Text(HDC dc, const std::wstring& text, RECT rect, int size, int weight, COLORREF color, UINT format = DT_LEFT | DT_VCENTER) {
    HFONT old{}; Font(dc, size, weight, color, old); DrawTextW(dc, text.c_str(), -1, &rect, format);
    HFONT current = static_cast<HFONT>(SelectObject(dc, old)); DeleteObject(current);
}

void Box(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius = 12) {
    HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}

void GlassButton(HDC dc, RECT rect, const wchar_t* label, int id, bool primary = false) {
    const bool pulsing = g_clickedButton == id && g_clickTicks > 0;
    COLORREF fill = primary ? (pulsing ? RGB(72, 158, 255) : RGB(35, 112, 235)) : (pulsing ? RGB(43, 91, 153) : RGB(18, 39, 68));
    COLORREF border = primary ? kAccentBright : RGB(57, 105, 157);
    RECT draw = rect;
    if (pulsing) { ++draw.top; ++draw.bottom; }
    Box(dc, draw, fill, border, (draw.bottom - draw.top) / 2);
    Text(dc, label, draw, primary ? 25 : 15, FW_BOLD, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PulseButton(int id) {
    g_clickedButton = id;
    g_clickTicks = 7;
    SetTimer(g_window, 2, 30, nullptr);
    InvalidateRect(g_window, nullptr, FALSE);
}

void StatusRow(HDC dc, const wchar_t* label, const wchar_t* state, int y, bool on, bool waiting = false, int x = 42, int right = 512) {
    Text(dc, L"o", RECT{x, y - 1, x + 24, y + 27}, 18, FW_BOLD, StatusColor(on, waiting), DT_CENTER | DT_VCENTER);
    Text(dc, label, RECT{x + 36, y, right - 150, y + 26}, 15, FW_NORMAL, kText);
    Text(dc, state, RECT{right - 136, y, right - 28, y + 26}, 14, FW_NORMAL, StatusColor(on, waiting), DT_RIGHT | DT_VCENTER);
    HBRUSH b = CreateSolidBrush(StatusColor(on, waiting)); RECT dot{right - 18, y + 8, right - 6, y + 20}; HGDIOBJ oldBrush = SelectObject(dc, b); Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom); SelectObject(dc, oldBrush); DeleteObject(b);
}

void PaintMain(HDC dc, RECT client) {
    HBRUSH bg = CreateSolidBrush(kBg); FillRect(dc, &client, bg); DeleteObject(bg);
    const int w = client.right, h = client.bottom;
    const int margin = std::max(48, w / 20), top = 292, bottom = h - 22;
    const int left = margin, gap = 26, right = w - margin;
    const int leftRight = left + (right - left - gap) * 58 / 100;
    const int panelTop = 292, panelBottom = h - 112;

    Text(dc, L"BIGSCREEN", RECT{margin, 82, margin + 420, 150}, 48, FW_BOLD, kText);
    Text(dc, L"NOVR", RECT{margin + 430, 82, margin + 650, 150}, 48, FW_BOLD, kAccentBright);
    Text(dc, L"Bigscreen on your PC - no headset required", RECT{margin + 2, 158, margin + 620, 192}, 20, FW_NORMAL, kMuted);
    Box(dc, RECT{margin, 210, margin + 154, 250}, kPanel2, kBorder, 8); Text(dc, L"v1.0 Preview", RECT{margin + 18, 210, margin + 140, 250}, 14, FW_NORMAL, kText, DT_CENTER | DT_VCENTER);
    Text(dc, L"Unofficial community project", RECT{margin + 176, 210, margin + 420, 250}, 16, FW_NORMAL, kMuted, DT_LEFT | DT_VCENTER);
    Box(dc, RECT{margin, 18, margin + 292, 70}, kPanel2, kBorder, 10); Text(dc, L"[o]  SteamVR", RECT{margin + 20, 20, margin + 164, 68}, 16, FW_BOLD, kText, DT_LEFT | DT_VCENTER); Text(dc, ProcessExists(L"vrmonitor.exe") ? L"Running" : L"Offline", RECT{margin + 188, 20, margin + 274, 68}, 15, FW_NORMAL, StatusColor(ProcessExists(L"vrmonitor.exe"), false), DT_RIGHT | DT_VCENTER);

    Box(dc, RECT{left, panelTop, leftRight, panelBottom}, kPanel, kBorder, 16);
    Text(dc, L"[ ]  VIEW MODE", RECT{left + 26, panelTop + 24, leftRight - 26, panelTop + 62}, 18, FW_BOLD, kMuted);
    g_firstRect = RECT{left + 26, panelTop + 76, left + (leftRight-left)/2 - 8, panelTop + 244}; g_thirdRect = RECT{left + (leftRight-left)/2 + 8, panelTop + 76, leftRight - 26, panelTop + 244};
    Box(dc, g_firstRect, !g_thirdPerson ? RGB(25, 62, 113) : kPanel2, !g_thirdPerson ? kAccentBright : kBorder, 14);
    Box(dc, g_thirdRect, g_thirdPerson ? RGB(25, 62, 113) : kPanel2, g_thirdPerson ? kAccentBright : kBorder, 14);
    Text(dc, L"O", RECT{g_firstRect.left + 38, g_firstRect.top + 38, g_firstRect.left + 114, g_firstRect.bottom - 28}, 38, FW_NORMAL, kText, DT_CENTER | DT_VCENTER);
    Text(dc, L"A", RECT{g_thirdRect.left + 38, g_thirdRect.top + 38, g_thirdRect.left + 114, g_thirdRect.bottom - 28}, 42, FW_NORMAL, kAccentBright, DT_CENTER | DT_VCENTER);
    Text(dc, L"FIRST PERSON", RECT{g_firstRect.left + 132, g_firstRect.top + 42, g_firstRect.right - 18, g_firstRect.top + 82}, 19, FW_BOLD, kText);
    Text(dc, L"See through your", RECT{g_firstRect.left + 132, g_firstRect.top + 88, g_firstRect.right - 18, g_firstRect.top + 114}, 15, FW_NORMAL, kMuted);
    Text(dc, L"avatar's eyes", RECT{g_firstRect.left + 132, g_firstRect.top + 114, g_firstRect.right - 18, g_firstRect.top + 140}, 15, FW_NORMAL, kMuted);
    Text(dc, L"THIRD PERSON", RECT{g_thirdRect.left + 132, g_thirdRect.top + 42, g_thirdRect.right - 18, g_thirdRect.top + 82}, 19, FW_BOLD, kText);
    Text(dc, L"Follow your avatar", RECT{g_thirdRect.left + 132, g_thirdRect.top + 88, g_thirdRect.right - 18, g_thirdRect.top + 114}, 15, FW_NORMAL, kMuted);
    Text(dc, L"from behind", RECT{g_thirdRect.left + 132, g_thirdRect.top + 114, g_thirdRect.right - 18, g_thirdRect.top + 140}, 15, FW_NORMAL, kMuted);
    Text(dc, g_thirdPerson ? L"OK" : L"", RECT{g_thirdRect.right - 52, g_thirdRect.top + 18, g_thirdRect.right - 18, g_thirdRect.top + 52}, 14, FW_BOLD, kAccentBright, DT_CENTER | DT_VCENTER);
    Text(dc, g_thirdPerson ? L">  Selected: THIRD PERSON" : L">  Selected: FIRST PERSON", RECT{left + 26, panelTop + 266, leftRight - 26, panelTop + 304}, 17, FW_BOLD, kAccentBright);
    g_playRect = RECT{left + 26, panelTop + 350, leftRight - 26, panelTop + 456};
    GlassButton(dc, g_playRect, g_playing ? L"STOP BIGSCREEN" : L"PLAY BIGSCREEN", 1, true);
    Text(dc, L"i  Press Play to start all required components and launch Bigscreen.", RECT{left + 26, panelTop + 476, leftRight - 26, panelTop + 516}, 14, FW_NORMAL, kMuted, DT_CENTER | DT_VCENTER);

    const int statusLeft = leftRight + gap;
    Box(dc, RECT{statusLeft, panelTop, right, panelBottom}, kPanel, kBorder, 16);
    Text(dc, L"[~]  SYSTEM STATUS", RECT{statusLeft + 26, panelTop + 24, right - 26, panelTop + 62}, 18, FW_BOLD, kMuted);
    const int rowTop = panelTop + 76;
    Box(dc, RECT{statusLeft + 18, rowTop, right - 18, rowTop + 340}, kPanel2, kBorder, 14);
    StatusRow(dc, L"SteamVR", ProcessExists(L"vrcompositor.exe") ? L"Running" : (ProcessExists(L"vrmonitor.exe") ? L"Running" : L"Offline"), rowTop + 24, ProcessExists(L"vrmonitor.exe"), false, statusLeft + 36, right - 36);
    StatusRow(dc, L"Bigscreen", ProcessExists(L"Bigscreen.exe") ? L"Running" : L"Offline", rowTop + 80, ProcessExists(L"Bigscreen.exe"), false, statusLeft + 36, right - 36);
    StatusRow(dc, L"NOVR Bridge", ProcessExists(L"BigscreenDesktopBridge.exe") ? L"Running" : L"Ready", rowTop + 136, ProcessExists(L"BigscreenDesktopBridge.exe"), true, statusLeft + 36, right - 36);
    StatusRow(dc, L"Desktop Viewer", ProcessExists(L"BigscreenDesktopViewer.exe") ? L"Running" : L"Offline", rowTop + 192, ProcessExists(L"BigscreenDesktopViewer.exe"), false, statusLeft + 36, right - 36);
    StatusRow(dc, L"Third-Person Camera", ProcessExists(L"BigscreenDesktopViewer.exe") && g_thirdPerson ? L"Selected" : L"Waiting", rowTop + 248, false, true, statusLeft + 36, right - 36);
    g_settingsRect = RECT{statusLeft + 18, rowTop + 370, right - 18, rowTop + 430};
    GlassButton(dc, g_settingsRect, L"VIEW LOGS", 2);

    Box(dc, RECT{left, bottom - 70, right, bottom}, kPanel, kBorder, 12);
    Text(dc, L"+  No Headset Required", RECT{left + 28, bottom - 70, left + 270, bottom}, 15, FW_NORMAL, kMuted, DT_LEFT | DT_VCENTER);
    Text(dc, L"+  Private & Local", RECT{left + 316, bottom - 70, left + 540, bottom}, 15, FW_NORMAL, kMuted, DT_LEFT | DT_VCENTER);
    Text(dc, L"+  Community Project", RECT{left + 590, bottom - 70, left + 850, bottom}, 15, FW_NORMAL, kMuted, DT_LEFT | DT_VCENTER);
    g_settingsRect = RECT{right - 214, bottom - 58, right - 18, bottom - 12};
    GlassButton(dc, g_settingsRect, L"SETTINGS", 3);
}

void PaintSettings(HDC dc, RECT client) {
    HBRUSH overlay = CreateSolidBrush(RGB(9, 11, 15)); FillRect(dc, &client, overlay); DeleteObject(overlay);
    Box(dc, RECT{120, 80, 780, 520}, kPanel, kBorder, 16);
    Text(dc, L"SETTINGS", RECT{158, 112, 600, 154}, 25, FW_BOLD, kText);
    Text(dc, L"Preview options", RECT{160, 154, 600, 184}, 14, FW_NORMAL, kMuted);
    g_autoSteamRect = RECT{160, 216, 720, 256}; g_autoBigscreenRect = RECT{160, 274, 720, 314}; g_keepTopRect = RECT{160, 332, 720, 372};
    const auto check = [&](RECT r, bool value, const wchar_t* label) { Box(dc, RECT{r.left, r.top + 4, r.left + 24, r.top + 28}, value ? kAccent : kPanel2, value ? kAccent : kBorder, 4); Text(dc, value ? L"X" : L"", RECT{r.left, r.top + 2, r.left + 24, r.top + 28}, 14, FW_BOLD, kText, DT_CENTER | DT_VCENTER); Text(dc, label, RECT{r.left + 40, r.top, r.right, r.bottom}, 15, FW_NORMAL, kText); };
    check(g_autoSteamRect, g_launchSteamVR, L"Launch SteamVR automatically");
    check(g_autoBigscreenRect, g_launchBigscreen, L"Launch Bigscreen automatically when beside the app");
    check(g_keepTopRect, g_keepViewerTop, L"Keep the viewer on top");
    Text(dc, L"Start view", RECT{160, 400, 320, 430}, 14, FW_BOLD, kMuted);
    Text(dc, g_thirdPerson ? L"Third Person" : L"First Person", RECT{340, 400, 600, 430}, 14, FW_NORMAL, kText);
    g_settingsCloseRect = RECT{590, 450, 720, 492}; GlassButton(dc, g_settingsCloseRect, L"DONE", 4, true);
}

void Paint(HDC dc) {
    RECT client{}; GetClientRect(g_window, &client);
    if (g_settingsOpen) PaintSettings(dc, client); else PaintMain(dc, client);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        Paint(buffer);
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (message == WM_TIMER) {
        if (wParam == 2 && g_clickTicks > 0) {
            --g_clickTicks;
            if (g_clickTicks == 0) { KillTimer(hwnd, 2); g_clickedButton = 0; }
        }
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    if (message == WM_LBUTTONUP) {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_settingsOpen) {
            if (PtInRect(&g_autoSteamRect, p)) g_launchSteamVR = !g_launchSteamVR;
            else if (PtInRect(&g_autoBigscreenRect, p)) g_launchBigscreen = !g_launchBigscreen;
            else if (PtInRect(&g_keepTopRect, p)) g_keepViewerTop = !g_keepViewerTop;
            else if (PtInRect(&g_settingsCloseRect, p)) { PulseButton(4); g_settingsOpen = false; SaveSettings(); }
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        if (PtInRect(&g_firstRect, p)) { PulseButton(5); ApplyView(false); }
        else if (PtInRect(&g_thirdRect, p)) { PulseButton(6); ApplyView(true); }
        else if (PtInRect(&g_playRect, p)) { PulseButton(1); g_playing ? Stop() : Play(); }
        else if (PtInRect(&g_settingsRect, p)) { PulseButton(3); g_settingsOpen = true; }
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    if (message == WM_CLOSE) { SaveSettings(); DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { KillTimer(hwnd, 1); KillTimer(hwnd, 2); PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    LoadSettings();
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = WindowProc; wc.lpszClassName = L"BigscreenNOVRPreview"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Bigscreen NOVR", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1536, 1024, nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    BOOL darkTitleBar = TRUE;
    constexpr DWORD kUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(g_window, kUseImmersiveDarkMode, &darkTitleBar, sizeof(darkTitleBar));
    ShowWindow(g_window, show); UpdateWindow(g_window); g_timer = SetTimer(g_window, 1, 750, nullptr);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
