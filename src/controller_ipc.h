#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_desktop_controller_ipc {

constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopControllerInput";
constexpr uint32_t kMagic = 0x42445343; // BDSC
constexpr uint32_t kVersion = 1;

enum Button : uint32_t {
    Button_A = 1u << 0,
    Button_B = 1u << 1,
    Button_X = 1u << 2,
    Button_Y = 1u << 3,
    Button_LeftBumper = 1u << 4,
    Button_RightBumper = 1u << 5,
    Button_LeftStick = 1u << 6,
    Button_RightStick = 1u << 7,
    Button_View = 1u << 8,
    Button_Menu = 1u << 9,
};

struct ControllerState {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    volatile LONG sequence = 0;
    uint32_t connected = 0;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    uint32_t buttons = 0;
    uint32_t dpad = 8;
};

inline void Publish(ControllerState* state, bool connected, float leftX, float leftY,
                    float rightX, float rightY, float leftTrigger, float rightTrigger,
                    uint32_t buttons, uint32_t dpad) {
    InterlockedIncrement(&state->sequence);
    state->connected = connected ? 1u : 0u;
    state->leftX = leftX;
    state->leftY = leftY;
    state->rightX = rightX;
    state->rightY = rightY;
    state->leftTrigger = leftTrigger;
    state->rightTrigger = rightTrigger;
    state->buttons = buttons;
    state->dpad = dpad;
    InterlockedIncrement(&state->sequence);
}

inline bool Read(const ControllerState* state, ControllerState& out) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG first = state->sequence;
        if (first & 1) continue;
        out = *state;
        const LONG second = state->sequence;
        if (first == second && !(second & 1) && out.magic == kMagic && out.version == kVersion) return true;
    }
    return false;
}

}
