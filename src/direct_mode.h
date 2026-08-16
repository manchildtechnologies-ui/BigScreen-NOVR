#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "openvr_driver.h"

static_assert(sizeof(vr::SharedTextureHandle_t) == 8, "Unexpected OpenVR shared handle ABI");
static_assert(sizeof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t) == 152,
              "Unexpected OpenVR SubmitLayerPerEye_t ABI");
static_assert(alignof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t) == 8,
              "Unexpected OpenVR SubmitLayerPerEye_t alignment");
static_assert(offsetof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t, hTexture) == 0);
static_assert(offsetof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t, hDepthTexture) == 8);
static_assert(offsetof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t, bounds) == 16);
static_assert(offsetof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t, mProjection) == 32);
static_assert(offsetof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t, mHmdPose) == 96);
static_assert(offsetof(vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t,
                       flHmdPosePredictionTimeInSecondsFromNow) == 144);

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIKeyedMutex;
struct IDXGIAdapter1;

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
    struct TextureSet {
        uint32_t pid = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
        uint32_t sampleCount = 0;
        uint32_t bindFlags = 0;
        uint32_t miscFlags = 0;
        uint32_t nextIndex = 0;
        vr::SharedTextureHandle_t handles[3]{};
        ID3D11Texture2D* textures[3]{};

        ~TextureSet();
    };

    void LogOnce(bool& flag, const char* message);
    void ReleaseAllTextureSets();
    bool EnsureDiagnosticStaging(TextureSet* set);
    bool CaptureRequested() const;
    bool WriteRawDiagnosticFrame(const TextureSet* set, uint32_t textureIndex);
    void ReleaseSyncTexture();

    IDXGIAdapter1* adapter_ = nullptr;
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;
    uint64_t adapterLuid_ = 0;
    std::mutex textureMutex_;
    std::vector<std::unique_ptr<TextureSet>> textureSets_;
    std::unordered_map<vr::SharedTextureHandle_t, TextureSet*> handleIndex_;
    struct SubmitDiagnostic {
        vr::SharedTextureHandle_t handles[2]{};
        uint32_t indices[2]{};
        bool mapped[2]{};
        vr::VRTextureBounds_t bounds[2]{};
        float predictionSeconds[2]{};
    } lastSubmit_;
    ID3D11Texture2D* diagnosticStaging_ = nullptr;
    uint32_t diagnosticWidth_ = 0;
    uint32_t diagnosticHeight_ = 0;
    uint32_t diagnosticFormat_ = 0;
    uint32_t diagnosticSamples_ = 0;
    vr::SharedTextureHandle_t syncHandle_ = 0;
    ID3D11Texture2D* syncTexture_ = nullptr;
    IDXGIKeyedMutex* syncMutex_ = nullptr;
    uint64_t diagnosticSequence_ = 0;
    bool loggedCreate_ = false;
    bool loggedDestroy_ = false;
    bool loggedDestroyAll_ = false;
    bool loggedNext_ = false;
    bool loggedSubmit_ = false;
    bool loggedPresent_ = false;
    bool loggedPost_ = false;
    bool loggedTiming_ = false;
    bool loggedSync_ = false;
    bool loggedCapture_ = false;
};
