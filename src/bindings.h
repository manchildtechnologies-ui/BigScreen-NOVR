#pragma once

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace bigscreen_bindings {

enum class Action {
    MoveForward, MoveBackward, MoveLeft, MoveRight,
    LookToggle, Reset, Exit,
    VRTrigger, VRGrip, VRMenu, VRTrackpadClick, MicToggle,
    ArmUp, ArmDown, ArmLeft, ArmRight,
};

enum class Device { Keyboard, Mouse, Gamepad };

struct Binding {
    Device device = Device::Keyboard;
    int code = 0;
};

inline const wchar_t* Name(Action action) {
    switch (action) {
    case Action::MoveForward: return L"Move Forward";
    case Action::MoveBackward: return L"Move Backward";
    case Action::MoveLeft: return L"Move Left";
    case Action::MoveRight: return L"Move Right";
    case Action::LookToggle: return L"Mouse Look Toggle";
    case Action::Reset: return L"Recenter";
    case Action::Exit: return L"Release / Exit";
    case Action::VRTrigger: return L"VR Trigger";
    case Action::VRGrip: return L"VR Grip";
    case Action::VRMenu: return L"VR Menu";
    case Action::VRTrackpadClick: return L"VR Trackpad Click";
    case Action::MicToggle: return L"Mic Toggle";
    case Action::ArmUp: return L"Arm Up";
    case Action::ArmDown: return L"Arm Down";
    case Action::ArmLeft: return L"Arm Left";
    case Action::ArmRight: return L"Arm Right";
    }
    return L"Unknown";
}

inline const wchar_t* DeviceName(Device device) {
    switch (device) {
    case Device::Keyboard: return L"Keyboard";
    case Device::Mouse: return L"Mouse";
    case Device::Gamepad: return L"Gamepad";
    }
    return L"Unknown";
}

inline const std::map<Action, Binding>& Defaults() {
    static const std::map<Action, Binding> defaults = {
        {Action::MoveForward, {Device::Keyboard, VK_UP}},
        {Action::MoveBackward, {Device::Keyboard, VK_DOWN}},
        {Action::MoveLeft, {Device::Keyboard, VK_LEFT}},
        {Action::MoveRight, {Device::Keyboard, VK_RIGHT}},
        {Action::LookToggle, {Device::Keyboard, VK_F8}},
        {Action::Reset, {Device::Keyboard, 'R'}},
        {Action::Exit, {Device::Keyboard, VK_ESCAPE}},
        {Action::VRTrigger, {Device::Gamepad, 0}},
        {Action::VRGrip, {Device::Gamepad, 5}},
        {Action::VRMenu, {Device::Gamepad, 8}},
        {Action::VRTrackpadClick, {Device::Gamepad, 7}},
        {Action::MicToggle, {Device::Gamepad, 6}},
        {Action::ArmUp, {Device::Gamepad, 0x40}},
        {Action::ArmDown, {Device::Gamepad, 0x80}},
        {Action::ArmLeft, {Device::Gamepad, 0x100}},
        {Action::ArmRight, {Device::Gamepad, 0x200}},
    };
    return defaults;
}

inline const wchar_t* FileName() { return L"bindings.json"; }

inline std::filesystem::path SettingsPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return std::filesystem::path(buffer) / L"BigscreenDesktopNoVR" / FileName();
    return std::filesystem::path(FileName());
}

inline std::wstring BindingLabel(const Binding& binding) {
    if (binding.device == Device::Keyboard) {
        if (binding.code == VK_UP) return L"Up Arrow";
        if (binding.code == VK_DOWN) return L"Down Arrow";
        if (binding.code == VK_LEFT) return L"Left Arrow";
        if (binding.code == VK_RIGHT) return L"Right Arrow";
        if (binding.code == VK_ESCAPE) return L"Escape";
        if (binding.code == VK_F8) return L"F8";
        if (binding.code >= 'A' && binding.code <= 'Z') return std::wstring(1, static_cast<wchar_t>(binding.code));
        return L"Keyboard 0x" + std::to_wstring(binding.code);
    }
    if (binding.device == Device::Mouse) {
        if (binding.code == 1) return L"Mouse Left";
        if (binding.code == 2) return L"Mouse Right";
        if (binding.code == 3) return L"Mouse Middle";
        return L"Mouse";
    }
    switch (binding.code) {
    case 0: return L"Gamepad A";
    case 1: return L"Gamepad B";
    case 2: return L"Gamepad X";
    case 3: return L"Gamepad Y";
    case 5: return L"Gamepad RT";
    case 6: return L"Left Stick Click";
    case 7: return L"Right Stick Click";
    case 8: return L"Gamepad View";
    case 9: return L"Gamepad Menu";
    case 0x40: return L"D-pad Up";
    case 0x80: return L"D-pad Down";
    case 0x100: return L"D-pad Left";
    case 0x200: return L"D-pad Right";
    default: return L"Gamepad Input";
    }
}

inline std::map<Action, Binding> Load(const std::filesystem::path& path) {
    auto bindings = Defaults();
    std::wifstream input(path);
    if (!input) return bindings;
    std::wstring line;
    while (std::getline(input, line)) {
        for (const auto& [action, unused] : Defaults()) {
            const std::wstring name = Name(action);
            if (line.find(L"\"" + std::wstring(name) + L"\"") == std::wstring::npos) continue;
            const size_t deviceStart = line.find(L"\"device\"");
            const size_t codeStart = line.find(L"\"code\"");
            if (deviceStart == std::wstring::npos || codeStart == std::wstring::npos) continue;
            Binding binding = unused;
            if (line.substr(deviceStart, 48).find(L"Mouse") != std::wstring::npos) binding.device = Device::Mouse;
            else if (line.substr(deviceStart, 48).find(L"Gamepad") != std::wstring::npos) binding.device = Device::Gamepad;
            else binding.device = Device::Keyboard;
            const size_t colon = line.find(L':', codeStart);
            if (colon == std::wstring::npos) continue;
            try { binding.code = std::stoi(line.substr(colon + 1)); } catch (...) { continue; }
            bindings[action] = binding;
        }
    }
    return bindings;
}

inline bool Save(const std::filesystem::path& path, const std::map<Action, Binding>& bindings) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::wofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << L"{\n  \"bindings\": {\n";
    bool first = true;
    for (const auto& [action, binding] : bindings) {
        if (!first) output << L",\n";
        first = false;
        output << L"    \"" << Name(action) << L"\": { \"device\": \"" << DeviceName(binding.device)
               << L"\", \"code\": " << binding.code << L" }";
    }
    output << L"\n  }\n}\n";
    return true;
}

inline bool Pressed(const Binding& binding, const BYTE* keyboardState, uint32_t buttons, uint32_t dpad) {
    if (binding.device == Device::Keyboard) return keyboardState && (keyboardState[binding.code] & 0x80) != 0;
    if (binding.device == Device::Gamepad) {
        if (binding.code >= 0x40) return (dpad & static_cast<uint32_t>(binding.code)) != 0;
        return (buttons & (1u << binding.code)) != 0;
    }
    return false;
}

}
