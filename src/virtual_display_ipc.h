#pragma once

#include <Windows.h>
#include <cstdint>

namespace bigscreen_virtual_display_ipc {
constexpr wchar_t kMappingName[] = L"Local\\BigscreenDesktopVirtualDisplayFrame";
constexpr uint32_t kMagic = 0x56445246; // VDRF
constexpr uint32_t kMaxWidth = 3840;
constexpr uint32_t kMaxHeight = 2160;
constexpr uint32_t kMaxBytes = kMaxWidth * kMaxHeight * 4;

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic = kMagic;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    uint64_t frameId = 0;
    volatile LONG sequence = 0;
};
#pragma pack(pop)

constexpr size_t kMappingSize = sizeof(FrameHeader) + kMaxBytes;
}
