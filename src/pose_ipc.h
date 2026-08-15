#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_desktop_ipc {

constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopPose";
constexpr uint32_t kMagic = 0x42534450; // BSDP
constexpr uint32_t kVersion = 1;

struct PoseState {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    LONG sequence = 0;
    LONG active = 0;
    double yaw = 0.0;
    double pitch = 0.0;
    ULONGLONG timestamp = 0;
};

static_assert(sizeof(PoseState) == 40, "PoseState layout changed");

inline void Publish(PoseState* state, double yaw, double pitch, bool active) {
    const LONG next = state->sequence + 1;
    InterlockedExchange(&state->sequence, next | 1);
    state->yaw = yaw;
    state->pitch = pitch;
    state->active = active ? 1 : 0;
    state->timestamp = GetTickCount64();
    InterlockedExchange(&state->sequence, (next + 1) & ~1L);
}

inline bool Read(const PoseState* state, double& yaw, double& pitch, bool& active) {
    if (!state || state->magic != kMagic || state->version != kVersion) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = state->sequence;
        if (before & 1) continue;
        const double readYaw = state->yaw;
        const double readPitch = state->pitch;
        const bool readActive = state->active != 0;
        const LONG after = state->sequence;
        if (before == after && !(after & 1)) {
            yaw = readYaw;
            pitch = readPitch;
            active = readActive;
            return true;
        }
    }
    return false;
}

} // namespace bigscreen_desktop_ipc
