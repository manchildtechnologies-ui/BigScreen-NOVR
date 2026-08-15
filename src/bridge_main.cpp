#include "pose_ipc.h"

#include <Windows.h>
#include <conio.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kPitchLimit = 80.0 * kPi / 180.0;
constexpr double kYawRate = 1.8;
constexpr double kPitchRate = 1.5;

void PrintStatus(double yaw, double pitch, bool connected) {
    std::printf("\rYaw: %7.2f deg   Pitch: %7.2f deg   IPC: %s   [Arrows rotate, R reset, ESC exit]   ",
                yaw * 180.0 / kPi, pitch * 180.0 / kPi, connected ? "active" : "starting");
    std::fflush(stdout);
}
}

int main() {
    using namespace bigscreen_desktop_ipc;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, static_cast<DWORD>(sizeof(PoseState)), kMappingName);
    if (!mapping) {
        std::fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
        return 1;
    }
    auto* state = static_cast<PoseState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PoseState)));
    if (!state) {
        std::fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    *state = PoseState{};
    Publish(state, 0.0, 0.0, true);
    std::printf("BigscreenDesktopBridge keyboard input\n");
    std::printf("Shared memory: %ls\n", kMappingName);

    bool running = true;
    double yaw = 0.0;
    double pitch = 0.0;
    ULONGLONG lastTick = GetTickCount64();
    ULONGLONG lastDisplay = 0;
    while (running) {
        const ULONGLONG now = GetTickCount64();
        const double dt = std::clamp(static_cast<double>(now - lastTick) / 1000.0, 0.0, 0.1);
        lastTick = now;

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) running = false;
        if (GetAsyncKeyState('R') & 0x0001) {
            yaw = 0.0;
            pitch = 0.0;
        }
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) yaw -= kYawRate * dt;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) yaw += kYawRate * dt;
        if (GetAsyncKeyState(VK_UP) & 0x8000) pitch += kPitchRate * dt;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) pitch -= kPitchRate * dt;
        pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);
        Publish(state, yaw, pitch, running);

        if (now - lastDisplay >= 250) {
            PrintStatus(yaw, pitch, true);
            lastDisplay = now;
        }
        Sleep(10);
    }

    Publish(state, yaw, pitch, false);
    std::printf("\nBridge exited.\n");
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return 0;
}
