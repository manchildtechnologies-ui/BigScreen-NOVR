#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_third_person_gpu {

constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopThirdPersonGPU";
constexpr uint32_t kMagic = 0x42544750; // BTGP
constexpr uint32_t kVersion = 1;

#pragma pack(push, 1)
struct State {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    LONG valid = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t reserved = 0;
    uint64_t sharedHandle = 0;
    uint64_t adapterLuid = 0;
    uint64_t generation = 0;
    LONG64 frameSequence = 0;
    uint64_t lastCopyMicros = 0;
};
#pragma pack(pop)

static_assert(sizeof(State) == 68, "Third-person GPU IPC layout changed");

} // namespace bigscreen_third_person_gpu
