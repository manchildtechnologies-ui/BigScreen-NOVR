#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_live_frame_ipc {

constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopLiveFrame";
constexpr uint32_t kMagic = 0x424C4631; // BLF1
constexpr uint32_t kVersion = 1;

struct LiveFrameState {
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

static_assert(sizeof(LiveFrameState) == 72, "LiveFrameState layout changed");

} // namespace bigscreen_live_frame_ipc
