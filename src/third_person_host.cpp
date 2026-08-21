#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "frida-core.h"

namespace fs = std::filesystem;

namespace {
GMainLoop* g_loop = nullptr;
FridaSession* g_session = nullptr;
FridaScript* g_script = nullptr;
fs::path g_status_path;

void status(const char* value) {
    std::ofstream out(g_status_path, std::ios::trunc);
    if (out) out << value << "\n";
}

DWORD find_bigscreen() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"Bigscreen.exe") == 0) { pid = entry.th32ProcessID; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

bool process_exists(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD code = 0; const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process); return alive;
}

fs::path local_app_data() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return length ? fs::path(buffer) : fs::current_path();
}

std::string utf8(const fs::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string read_agent(const fs::path& path, const fs::path& runtime) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string dll = utf8(runtime / "BigscreenThirdPersonGpuExport.dll");
    const std::string report = utf8(local_app_data() / "BigscreenNOVR" / "BigscreenV24-GPU.txt");
    size_t pos = 0;
    const std::string dll_token = "BigscreenThirdPersonGpuExport.dll";
    while ((pos = source.find(dll_token, pos)) != std::string::npos) { source.replace(pos, dll_token.size(), dll); pos += dll.size(); }
    pos = 0;
    const std::string report_token = "BigscreenV24-GPU.txt";
    while ((pos = source.find(report_token, pos)) != std::string::npos) { source.replace(pos, report_token.size(), report); pos += report.size(); }
    return source;
}

void on_detached(FridaSession*, FridaSessionDetachReason, FridaCrash*, gpointer) {
    status("Error");
    if (g_loop) g_main_loop_quit(g_loop);
}

void on_message(FridaScript*, const gchar* message, GBytes*, gpointer) {
    if (!message) return;
    const std::string text(message);
    if (text.find("agent_ready") != std::string::npos || text.find("hooks_installed") != std::string::npos) status("Starting");
    if (text.find("running") != std::string::npos) status("Running");
    if (text.find("metadata_missing") != std::string::npos || text.find("objects_missing") != std::string::npos || text.find("activation_error") != std::string::npos || text.find("frame_error") != std::string::npos) status("Unavailable");
}

gboolean monitor_target(gpointer user_data) {
    const DWORD pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(user_data));
    if (!process_exists(pid)) { status("Offline"); if (g_loop) g_main_loop_quit(g_loop); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

int run_host(const fs::path& runtime) {
    fs::create_directories(local_app_data() / "BigscreenNOVR");
    g_status_path = local_app_data() / "BigscreenNOVR" / "third-person.status";
    status("Starting");

    DWORD pid = 0;
    for (int i = 0; i < 240 && !(pid = find_bigscreen()); ++i) Sleep(250);
    if (!pid) { status("Unavailable"); return 2; }
    const std::string agent = read_agent(runtime / "third_person_agent.js", runtime);
    if (agent.empty()) { status("Unavailable"); return 3; }

    GError* error = nullptr;
    frida_init();
    FridaDeviceManager* manager = frida_device_manager_new();
    FridaDeviceList* devices = frida_device_manager_enumerate_devices_sync(manager, nullptr, &error);
    if (error || !devices) { status("Error"); if (error) g_error_free(error); frida_unref(manager); frida_deinit(); return 4; }
    FridaDevice* local = nullptr;
    for (gint i = 0; i < frida_device_list_size(devices); ++i) {
        FridaDevice* device = frida_device_list_get(devices, i);
        if (frida_device_get_dtype(device) == FRIDA_DEVICE_TYPE_LOCAL) local = device; else frida_unref(device);
    }
    frida_unref(devices);
    if (!local) { status("Error"); frida_unref(manager); frida_deinit(); return 5; }
    g_session = frida_device_attach_sync(local, pid, nullptr, nullptr, &error);
    if (error || !g_session) { status("Unavailable"); if (error) g_error_free(error); frida_unref(local); frida_unref(manager); frida_deinit(); return 6; }
    g_loop = g_main_loop_new(nullptr, FALSE);
    g_signal_connect(g_session, "detached", G_CALLBACK(on_detached), nullptr);
    FridaScriptOptions* options = frida_script_options_new();
    frida_script_options_set_name(options, "bigscreen-novr-third-person");
    frida_script_options_set_runtime(options, FRIDA_SCRIPT_RUNTIME_QJS);
    g_script = frida_session_create_script_sync(g_session, agent.c_str(), options, nullptr, &error);
    g_object_unref(options);
    if (error || !g_script) { status("Error"); if (error) g_error_free(error); g_main_loop_unref(g_loop); frida_unref(g_session); frida_unref(local); frida_unref(manager); frida_deinit(); return 7; }
    g_signal_connect(g_script, "message", G_CALLBACK(on_message), nullptr);
    frida_script_load_sync(g_script, nullptr, &error);
    if (error) { status("Error"); g_error_free(error); }
    else status("Starting");
    g_timeout_add(500, monitor_target, reinterpret_cast<gpointer>(static_cast<uintptr_t>(pid)));
    if (!error) g_main_loop_run(g_loop);
    if (g_script) { frida_script_unload_sync(g_script, nullptr, nullptr); frida_unref(g_script); g_script = nullptr; }
    if (g_session) { frida_session_detach_sync(g_session, nullptr, nullptr); frida_unref(g_session); g_session = nullptr; }
    frida_unref(local); frida_device_manager_close_sync(manager, nullptr, nullptr); frida_unref(manager); g_main_loop_unref(g_loop); g_loop = nullptr; frida_deinit();
    status("Offline"); return error ? 8 : 0;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    return run_host(fs::path(module).parent_path());
}
