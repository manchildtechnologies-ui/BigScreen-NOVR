#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_desktop_ipc {

// V2 uses a distinct mapping name so a V1 40-byte mapping can never be
// interpreted as the larger X/Z pose layout.
constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopPoseV2";
constexpr uint32_t kMagic = 0x42534450; // BSDP
constexpr uint32_t kVersion = 2;

struct PoseState {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t size = 64;
    LONG sequence = 0;
    LONG active = 0;
    double positionX = 0.0;
    double positionZ = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
    ULONGLONG timestamp = 0;
};

static_assert(sizeof(PoseState) == 64, "PoseState layout changed");

inline void Publish(PoseState* state, double positionX, double positionZ,
                    double yaw, double pitch, bool active) {
    const LONG next = state->sequence + 1;
    InterlockedExchange(&state->sequence, next | 1);
    state->positionX = positionX;
    state->positionZ = positionZ;
    state->yaw = yaw;
    state->pitch = pitch;
    state->active = active ? 1 : 0;
    state->timestamp = GetTickCount64();
    InterlockedExchange(&state->sequence, (next + 1) & ~1L);
}

inline bool Read(const PoseState* state, double& positionX, double& positionZ,
                 double& yaw, double& pitch, bool& active) {
    if (!state || state->magic != kMagic || state->version != kVersion ||
        state->size != sizeof(PoseState)) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = state->sequence;
        if (before & 1) continue;
        const double readPositionX = state->positionX;
        const double readPositionZ = state->positionZ;
        const double readYaw = state->yaw;
        const double readPitch = state->pitch;
        const bool readActive = state->active != 0;
        const LONG after = state->sequence;
        if (before == after && !(after & 1)) {
            positionX = readPositionX;
            positionZ = readPositionZ;
            yaw = readYaw;
            pitch = readPitch;
            active = readActive;
            return true;
        }
    }
    return false;
}

} // namespace bigscreen_desktop_ipc
