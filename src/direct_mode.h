#pragma once

#include <Windows.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "openvr_driver.h"

struct ID3D11Device;
struct ID3D11Texture2D;
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

    IDXGIAdapter1* adapter_ = nullptr;
    ID3D11Device* d3dDevice_ = nullptr;
    uint64_t adapterLuid_ = 0;
    std::mutex textureMutex_;
    std::vector<std::unique_ptr<TextureSet>> textureSets_;
    std::unordered_map<vr::SharedTextureHandle_t, TextureSet*> handleIndex_;
    bool loggedCreate_ = false;
    bool loggedDestroy_ = false;
    bool loggedDestroyAll_ = false;
    bool loggedNext_ = false;
    bool loggedSubmit_ = false;
    bool loggedPresent_ = false;
    bool loggedPost_ = false;
    bool loggedTiming_ = false;
};
