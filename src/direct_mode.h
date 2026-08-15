#pragma once

#include <Windows.h>
#include <cstdint>

#include "openvr_driver.h"

class DirectMode final : public vr::IVRDriverDirectModeComponent {
public:
    DirectMode();
    ~DirectMode();

    bool InitializeGraphicsIdentity();
    uint64_t AdapterLuid() const { return adapterLuid_; }
    bool HasAdapter() const { return adapter_ != nullptr; }

    void CreateSwapTextureSet(uint32_t pid,
                              const SwapTextureSetDesc_t* desc,
                              SwapTextureSet_t* out) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
    void DestroyAllSwapTextureSets(uint32_t pid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                    uint32_t (*indices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t* throttling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) override;

private:
    void LogOnce(bool& flag, const char* message);

    struct IDXGIAdapter1* adapter_ = nullptr;
    void* d3dDevice_ = nullptr;
    uint64_t adapterLuid_ = 0;
    bool loggedCreate_ = false;
    bool loggedDestroy_ = false;
    bool loggedDestroyAll_ = false;
    bool loggedNext_ = false;
    bool loggedSubmit_ = false;
    bool loggedPresent_ = false;
    bool loggedPost_ = false;
    bool loggedTiming_ = false;
};
