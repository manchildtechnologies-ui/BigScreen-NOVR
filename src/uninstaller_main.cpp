#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <TlHelp32.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
fs::path ModuleDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

void StopProcess(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (process) { TerminateProcess(process, 0); CloseHandle(process); }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

fs::path FindSteam() {
    const wchar_t* p86 = _wgetenv(L"ProgramFiles(x86)");
    const wchar_t* p64 = _wgetenv(L"ProgramFiles");
    if (p86 && fs::exists(fs::path(p86) / L"Steam")) return fs::path(p86) / L"Steam";
    if (p64 && fs::exists(fs::path(p64) / L"Steam")) return fs::path(p64) / L"Steam";
    if (fs::exists(L"C:\\Steam")) return L"C:\\Steam";
    return {};
}

void ScheduleSelfDelete(const fs::path& install) {
    std::wstring command = L"/C ping 127.0.0.1 -n 2 > nul & rmdir /S /Q \"" + install.wstring() + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    if (!IsUserAnAdmin()) {
        const fs::path self = ModuleDir() / L"BigscreenDesktopUninstaller.exe";
        ShellExecuteW(nullptr, L"runas", self.c_str(), nullptr, self.parent_path().c_str(), SW_SHOWNORMAL);
        return 0;
    }
    if (MessageBoxW(nullptr,
                    L"Remove Bigscreen Desktop Bridge, its desktop shortcuts, and its SteamVR driver?\n\nBigscreen and SteamVR will not be removed.",
                    L"Uninstall Bigscreen Desktop Bridge", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;

    const fs::path install = ModuleDir();
    StopProcess(L"BigscreenDesktopViewer.exe");
    StopProcess(L"BigscreenDesktopBridge.exe");
    StopProcess(L"BigscreenDesktopLauncher.exe");

    const fs::path steam = FindSteam();
    if (!steam.empty()) {
        std::error_code error;
        fs::remove_all(steam / L"steamapps\\common\\SteamVR\\drivers\\driver_bigscreen_desktop", error);
    }
    wchar_t desktop[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktop);
    DeleteFileW((fs::path(desktop) / L"Start Bigscreen Desktop.lnk").c_str());
    DeleteFileW((fs::path(desktop) / L"Uninstall Bigscreen Desktop Bridge.lnk").c_str());
    ScheduleSelfDelete(install);
    MessageBoxW(nullptr, L"Bigscreen Desktop Bridge was uninstalled. Bigscreen and SteamVR were left installed.",
                L"Uninstall Complete", MB_OK | MB_ICONINFORMATION);
    return 0;
}
