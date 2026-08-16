#pragma once

#include <openvr_driver.h>
#include <Windows.h>
#include <d3d11.h>
#include <mutex>
#include <string>

class VirtualDisplay final : public vr::ITrackedDeviceServerDriver, public vr::IVRVirtualDisplay {
public:
    VirtualDisplay();
    ~VirtualDisplay();

    vr::EVRInitError Activate(uint32_t objectId) override;
    void Deactivate() override;
    void EnterStandby() override {}
    void* GetComponent(const char* componentNameAndVersion) override;
    void DebugRequest(const char* request, char* response, uint32_t responseSize) override;
    vr::DriverPose_t GetPose() override;

    void Present(const vr::PresentInfo_t* presentInfo, uint32_t presentInfoSize) override;
    void WaitForPresent() override;
    bool GetTimeSinceLastVsync(float* secondsSinceLastVsync, uint64_t* frameCounter) override;

private:
    bool InitializeGraphics();
    bool InitializeFrameMapping();
    void ReleaseGraphics();
    void CopyPendingFrame();

    uint32_t index_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t props_ = vr::k_ulInvalidPropertyContainer;
    std::mutex mutex_;
    vr::PresentInfo_t pending_{};
    bool pendingValid_ = false;
    uint64_t frameCounter_ = 0;
    LARGE_INTEGER clockFrequency_{};
    LARGE_INTEGER lastVsyncTicks_{};
    LARGE_INTEGER nextVsyncTicks_{};
    uint64_t logFrameCount_ = 0;
    uint64_t logStartTicks_ = 0;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    HANDLE mapping_ = nullptr;
    void* mapped_ = nullptr;
};
