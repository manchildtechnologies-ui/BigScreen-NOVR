#include "virtual_display.h"
#include "virtual_display_ipc.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "d3d11.lib")

VirtualDisplay::VirtualDisplay() {
    QueryPerformanceFrequency(&clockFrequency_);
    QueryPerformanceCounter(&lastVsyncTicks_);
    nextVsyncTicks_ = lastVsyncTicks_;
}

VirtualDisplay::~VirtualDisplay() {
    Deactivate();
}

vr::EVRInitError VirtualDisplay::Activate(uint32_t objectId) {
    index_ = objectId;
    props_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
    vr::VRProperties()->SetStringProperty(props_, vr::Prop_TrackingSystemName_String, "BigscreenDesktopBridge");
    vr::VRProperties()->SetStringProperty(props_, vr::Prop_ModelNumber_String, "Bigscreen Desktop Display Redirect");
    vr::VRProperties()->SetStringProperty(props_, vr::Prop_ManufacturerName_String, "BigscreenDesktopBridge");
    vr::VRProperties()->SetBoolProperty(props_, vr::Prop_HasVirtualDisplayComponent_Bool, true);
    vr::VRProperties()->SetFloatProperty(props_, vr::Prop_DisplayFrequency_Float, 90.0f);
    vr::VRProperties()->SetFloatProperty(props_, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.01111111f);
    if (!InitializeFrameMapping() || !InitializeGraphics()) {
        vr::VRDriverLog()->Log("Virtual Display initialization failed");
        return vr::VRInitError_Driver_Failed;
    }
    vr::VRDriverLog()->Log("Virtual Display activated: 90Hz, DisplayRedirect, IVRVirtualDisplay_002");
    return vr::VRInitError_None;
}

void VirtualDisplay::Deactivate() {
    ReleaseGraphics();
    index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void* VirtualDisplay::GetComponent(const char* componentNameAndVersion) {
    if (componentNameAndVersion && std::strcmp(componentNameAndVersion, vr::IVRVirtualDisplay_Version) == 0) {
        return static_cast<vr::IVRVirtualDisplay*>(this);
    }
    return nullptr;
}

void VirtualDisplay::DebugRequest(const char*, char* response, uint32_t responseSize) {
    if (responseSize) response[0] = '\0';
}

vr::DriverPose_t VirtualDisplay::GetPose() {
    vr::DriverPose_t pose{};
    pose.poseIsValid = false;
    pose.result = vr::TrackingResult_Uninitialized;
    pose.deviceIsConnected = true;
    pose.qWorldFromDriverRotation = {1, 0, 0, 0};
    pose.qDriverFromHeadRotation = {1, 0, 0, 0};
    pose.qRotation = {1, 0, 0, 0};
    return pose;
}

bool VirtualDisplay::InitializeFrameMapping() {
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(bigscreen_virtual_display_ipc::kMappingSize),
                                   bigscreen_virtual_display_ipc::kMappingName);
    if (!mapping_) return false;
    mapped_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, bigscreen_virtual_display_ipc::kMappingSize);
    if (!mapped_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
    std::memset(mapped_, 0, bigscreen_virtual_display_ipc::kMappingSize);
    auto* header = static_cast<bigscreen_virtual_display_ipc::FrameHeader*>(mapped_);
    header->magic = bigscreen_virtual_display_ipc::kMagic;
    return true;
}

bool VirtualDisplay::InitializeGraphics() {
    D3D_FEATURE_LEVEL level{};
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device_, &level, &context_));
}

void VirtualDisplay::ReleaseGraphics() {
    if (staging_) staging_->Release(); staging_ = nullptr;
    if (context_) context_->Release(); context_ = nullptr;
    if (device_) device_->Release(); device_ = nullptr;
    if (mapped_) UnmapViewOfFile(mapped_); mapped_ = nullptr;
    if (mapping_) CloseHandle(mapping_); mapping_ = nullptr;
}

void VirtualDisplay::Present(const vr::PresentInfo_t* presentInfo, uint32_t presentInfoSize) {
    if (!presentInfo || presentInfoSize < sizeof(vr::PresentInfo_t)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = *presentInfo;
    pendingValid_ = true;
    if (++logFrameCount_ == 1) {
        vr::VRDriverLog()->Log("Virtual Display Present() received first compositor frame");
    }
}

void VirtualDisplay::CopyPendingFrame() {
    vr::PresentInfo_t present{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pendingValid_) return;
        present = pending_;
        pendingValid_ = false;
    }
    if (!present.backbufferTextureHandle || !device_ || !context_ || !mapped_) return;

    ID3D11Resource* resource = nullptr;
    HRESULT hr = device_->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(present.backbufferTextureHandle)),
                                              IID_PPV_ARGS(&resource));
    ID3D11Texture2D* source = nullptr;
    if (SUCCEEDED(hr)) hr = resource->QueryInterface(IID_PPV_ARGS(&source));
    D3D11_TEXTURE2D_DESC desc{};
    if (SUCCEEDED(hr)) source->GetDesc(&desc);
    if (SUCCEEDED(hr) && (desc.Width > bigscreen_virtual_display_ipc::kMaxWidth ||
                          desc.Height > bigscreen_virtual_display_ipc::kMaxHeight || desc.SampleDesc.Count != 1)) {
        hr = E_INVALIDARG;
    }
    if (SUCCEEDED(hr)) {
        if (!staging_) {
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.BindFlags = 0;
            stagingDesc.MiscFlags = 0;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = device_->CreateTexture2D(&stagingDesc, nullptr, &staging_);
        }
        if (SUCCEEDED(hr)) {
            context_->CopyResource(staging_, source);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                auto* header = static_cast<bigscreen_virtual_display_ipc::FrameHeader*>(mapped_);
                auto* pixels = reinterpret_cast<uint8_t*>(header + 1);
                const size_t rowBytes = static_cast<size_t>(desc.Width) * 4;
                for (uint32_t y = 0; y < desc.Height; ++y) {
                    std::memcpy(pixels + static_cast<size_t>(y) * rowBytes,
                                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                rowBytes);
                }
                header->width = desc.Width;
                header->height = desc.Height;
                header->stride = static_cast<uint32_t>(rowBytes);
                header->format = desc.Format;
                header->frameId = present.nFrameId;
                MemoryBarrier();
                InterlockedIncrement(&header->sequence);
                context_->Unmap(staging_, 0);
                ++frameCounter_;
            }
        }
    }
    if (FAILED(hr)) {
        char line[256];
        sprintf_s(line, "Virtual Display frame copy failed hr=0x%08X handle=%llu", static_cast<unsigned>(hr),
                  static_cast<unsigned long long>(present.backbufferTextureHandle));
        vr::VRDriverLog()->Log(line);
    }
    if (source) source->Release();
    if (resource) resource->Release();
}

void VirtualDisplay::WaitForPresent() {
    CopyPendingFrame();
    const LONGLONG interval = clockFrequency_.QuadPart / 90;
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    if (nextVsyncTicks_.QuadPart <= now.QuadPart) nextVsyncTicks_.QuadPart = now.QuadPart + interval;
    else nextVsyncTicks_.QuadPart += interval;
    const LONGLONG remaining = nextVsyncTicks_.QuadPart - now.QuadPart;
    if (remaining > clockFrequency_.QuadPart / 1000) {
        Sleep(static_cast<DWORD>((remaining * 1000) / clockFrequency_.QuadPart));
    }
    QueryPerformanceCounter(&lastVsyncTicks_);
    ++frameCounter_;
}

bool VirtualDisplay::GetTimeSinceLastVsync(float* secondsSinceLastVsync, uint64_t* frameCounter) {
    if (!secondsSinceLastVsync || !frameCounter) return false;
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    *secondsSinceLastVsync = static_cast<float>(static_cast<double>(now.QuadPart - lastVsyncTicks_.QuadPart) /
                                                static_cast<double>(clockFrequency_.QuadPart));
    *frameCounter = frameCounter_;
    return true;
}
