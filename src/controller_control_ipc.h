#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_controller_control_ipc {

constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopControllerControl";
constexpr uint32_t kMagic = 0x42444343; // BDCC
constexpr uint32_t kVersion = 1;

enum class ViewRequest : uint32_t {
    None = 0,
    First = 1,
    Third = 2,
};

#pragma pack(push, 1)
struct State {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    LONG sequence = 0;
    uint32_t viewRequest = static_cast<uint32_t>(ViewRequest::None);
    uint32_t resetEpoch = 0;
    uint32_t virtualControllersEnabled = 1;
    int32_t cameraIndex = 0;
    float cameraDistance = 2.5f;
    float cameraHeight = 0.6f;
    float cameraFov = 60.0f;
};
#pragma pack(pop)

static_assert(sizeof(State) == 40, "Controller control IPC layout changed");

inline void Publish(State* state, uint32_t viewRequest, uint32_t resetEpoch,
                    bool virtualControllersEnabled, int32_t cameraIndex,
                    float cameraDistance, float cameraHeight, float cameraFov) {
    const LONG next = state->sequence + 1;
    InterlockedExchange(&state->sequence, next | 1);
    state->viewRequest = viewRequest;
    state->resetEpoch = resetEpoch;
    state->virtualControllersEnabled = virtualControllersEnabled ? 1u : 0u;
    state->cameraIndex = cameraIndex;
    state->cameraDistance = cameraDistance;
    state->cameraHeight = cameraHeight;
    state->cameraFov = cameraFov;
    InterlockedExchange(&state->sequence, (next + 1) & ~1L);
}

inline bool Read(const State* state, State& out) {
    if (!state || state->magic != kMagic || state->version != kVersion) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before = state->sequence;
        if (before & 1) continue;
        out = *state;
        const LONG after = state->sequence;
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

} // namespace bigscreen_controller_control_ipc
