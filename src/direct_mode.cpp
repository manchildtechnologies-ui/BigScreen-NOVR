#include "direct_mode.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cstdio>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

DirectMode::TextureSet::~TextureSet() {
    for (ID3D11Texture2D*& texture : textures) {
        if (texture) texture->Release();
        texture = nullptr;
    }
}

DirectMode::DirectMode() = default;

DirectMode::~DirectMode() {
    ReleaseAllTextureSets();
    if (d3dDevice_) {
        d3dDevice_->Release();
        d3dDevice_ = nullptr;
    }
    if (adapter_) {
        adapter_->Release();
        adapter_ = nullptr;
    }
}

void DirectMode::ReleaseAllTextureSets() {
    std::lock_guard<std::mutex> lock(textureMutex_);
    handleIndex_.clear();
    textureSets_.clear();
}

void DirectMode::LogOnce(bool& flag, const char* message) {
    if (!flag) {
        flag = true;
        vr::VRDriverLog()->Log(message);
    }
}

bool DirectMode::InitializeGraphicsIdentity() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    bool found = false;
    for (UINT index = 0; !found; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        if (!candidate) continue;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            desc.VendorId == 0x10DE) {
            adapter_ = candidate;
            adapterLuid_ = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                           static_cast<uint64_t>(desc.AdapterLuid.LowPart);
            found = true;
            char line[192];
            sprintf_s(line, "Direct Mode graphics adapter selected: %ls LUID=0x%016llX",
                      desc.Description, static_cast<unsigned long long>(adapterLuid_));
            vr::VRDriverLog()->Log(line);
        } else {
            candidate->Release();
        }
    }

    if (found) {
        D3D_FEATURE_LEVEL level{};
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        const HRESULT hr = D3D11CreateDevice(
            adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, &level, &context);
        if (context) context->Release();
        if (FAILED(hr)) {
            adapter_->Release();
            adapter_ = nullptr;
            adapterLuid_ = 0;
            return false;
        }
        d3dDevice_ = device;
    }

    factory->Release();
    return found;
}

void DirectMode::CreateSwapTextureSet(uint32_t pid,
                                      const SwapTextureSetDesc_t* desc,
                                      SwapTextureSet_t* out) {
    if (out) *out = {};
    if (!desc || !out || !d3dDevice_ || desc->nWidth == 0 || desc->nHeight == 0 || desc->nSampleCount == 0) return;

    constexpr UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    constexpr UINT miscFlags = D3D11_RESOURCE_MISC_SHARED;
    auto set = std::make_unique<TextureSet>();
    set->pid = pid;
    set->width = desc->nWidth;
    set->height = desc->nHeight;
    set->format = desc->nFormat;
    set->sampleCount = desc->nSampleCount;
    set->bindFlags = bindFlags;
    set->miscFlags = miscFlags;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = desc->nWidth;
    textureDesc.Height = desc->nHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = static_cast<DXGI_FORMAT>(desc->nFormat);
    textureDesc.SampleDesc.Count = desc->nSampleCount;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = bindFlags;
    textureDesc.CPUAccessFlags = 0;
    textureDesc.MiscFlags = miscFlags;

    for (size_t index = 0; index < 3; ++index) {
        HRESULT hr = d3dDevice_->CreateTexture2D(&textureDesc, nullptr, &set->textures[index]);
        if (FAILED(hr)) {
            char line[256];
            sprintf_s(line, "Direct Mode CreateSwapTextureSet texture[%zu] failed hr=0x%08X", index,
                      static_cast<unsigned>(hr));
            vr::VRDriverLog()->Log(line);
            return;
        }

        IDXGIResource* resource = nullptr;
        hr = set->textures[index]->QueryInterface(IID_PPV_ARGS(&resource));
        HANDLE sharedHandle = nullptr;
        if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&sharedHandle);
        if (resource) resource->Release();
        if (FAILED(hr) || !sharedHandle) {
            char line[256];
            sprintf_s(line, "Direct Mode CreateSwapTextureSet shared handle[%zu] failed hr=0x%08X", index,
                      static_cast<unsigned>(FAILED(hr) ? hr : E_HANDLE));
            vr::VRDriverLog()->Log(line);
            return;
        }
        set->handles[index] = static_cast<vr::SharedTextureHandle_t>(reinterpret_cast<uintptr_t>(sharedHandle));
        out->rSharedTextureHandles[index] = set->handles[index];
    }

    out->unTextureFlags = 0;
    TextureSet* stored = set.get();
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        for (const auto handle : set->handles) {
            if (handleIndex_.find(handle) != handleIndex_.end()) {
                *out = {};
                return;
            }
        }
        textureSets_.push_back(std::move(set));
        for (const auto handle : stored->handles) handleIndex_[handle] = stored;
    }

    const char* formatName = desc->nFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                 ? "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB"
                                 : "other";
    char line[384];
    sprintf_s(line, "Direct Mode CreateSwapTextureSet accepted pid=%u textures=3 size=%ux%u format=%u(%s) samples=%u bind=0x%X misc=0x%X handles=legacy",
              pid, desc->nWidth, desc->nHeight, desc->nFormat, formatName, desc->nSampleCount,
              bindFlags, miscFlags);
    vr::VRDriverLog()->Log(line);
}

void DirectMode::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) {
    std::lock_guard<std::mutex> lock(textureMutex_);
    auto found = handleIndex_.find(sharedTextureHandle);
    if (found == handleIndex_.end()) {
        LogOnce(loggedDestroy_, "Direct Mode DestroySwapTextureSet unknown handle");
        return;
    }
    TextureSet* target = found->second;
    for (const auto handle : target->handles) handleIndex_.erase(handle);
    textureSets_.erase(std::remove_if(textureSets_.begin(), textureSets_.end(),
                                      [target](const std::unique_ptr<TextureSet>& value) {
                                          return value.get() == target;
                                      }), textureSets_.end());
    LogOnce(loggedDestroy_, "Direct Mode DestroySwapTextureSet released texture set");
}

void DirectMode::DestroyAllSwapTextureSets(uint32_t pid) {
    std::lock_guard<std::mutex> lock(textureMutex_);
    for (auto iterator = textureSets_.begin(); iterator != textureSets_.end();) {
        if ((*iterator)->pid != pid) {
            ++iterator;
            continue;
        }
        for (const auto handle : (*iterator)->handles) handleIndex_.erase(handle);
        iterator = textureSets_.erase(iterator);
    }
    LogOnce(loggedDestroyAll_, "Direct Mode DestroyAllSwapTextureSets released process sets");
}

void DirectMode::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) {
    if (indices) {
        std::lock_guard<std::mutex> lock(textureMutex_);
        for (size_t eye = 0; eye < 2; ++eye) {
            auto found = handleIndex_.find(handles[eye]);
            if (found == handleIndex_.end()) {
                (*indices)[eye] = 0;
                continue;
            }
            TextureSet* set = found->second;
            set->nextIndex = (set->nextIndex + 1) % 3;
            (*indices)[eye] = set->nextIndex;
        }
    }
    LogOnce(loggedNext_, "Direct Mode GetNextSwapTextureSetIndex");
}

void DirectMode::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    SubmitDiagnostic diagnostic{};
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        for (size_t eye = 0; eye < 2; ++eye) {
            diagnostic.handles[eye] = perEye[eye].hTexture;
            diagnostic.bounds[eye] = perEye[eye].bounds;
            diagnostic.predictionSeconds[eye] = perEye[eye].flHmdPosePredictionTimeInSecondsFromNow;
            const auto found = handleIndex_.find(perEye[eye].hTexture);
            if (found != handleIndex_.end()) {
                diagnostic.mapped[eye] = true;
                for (uint32_t index = 0; index < 3; ++index) {
                    if (found->second->handles[index] == perEye[eye].hTexture) {
                        diagnostic.indices[eye] = index;
                        break;
                    }
                }
            }
        }
        lastSubmit_ = diagnostic;
    }
    if (!loggedSubmit_) {
        loggedSubmit_ = true;
        char line[384];
        sprintf_s(line, "Direct Mode SubmitLayer minimal left=0x%llX idx=%u mapped=%s right=0x%llX idx=%u mapped=%s bounds=(%.3f,%.3f)-(%.3f,%.3f)",
                  static_cast<unsigned long long>(diagnostic.handles[0]), diagnostic.indices[0],
                  diagnostic.mapped[0] ? "yes" : "no",
                  static_cast<unsigned long long>(diagnostic.handles[1]), diagnostic.indices[1],
                  diagnostic.mapped[1] ? "yes" : "no", diagnostic.bounds[0].uMin, diagnostic.bounds[0].vMin,
                  diagnostic.bounds[0].uMax, diagnostic.bounds[0].vMax);
        vr::VRDriverLog()->Log(line);
    }
}

void DirectMode::Present(vr::SharedTextureHandle_t) {
    LogOnce(loggedPresent_, "Direct Mode Present");
}

void DirectMode::PostPresent(const Throttling_t*) {
    LogOnce(loggedPost_, "Direct Mode PostPresent");
}

void DirectMode::GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) {
    if (timing) {
        timing->m_nNumFramePresents = 0;
        timing->m_nNumMisPresented = 0;
        timing->m_nNumDroppedFrames = 0;
        timing->m_nReprojectionFlags = 0;
    }
    LogOnce(loggedTiming_, "Direct Mode GetFrameTiming");
}
