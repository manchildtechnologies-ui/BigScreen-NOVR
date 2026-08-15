#include "direct_mode.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

DirectMode::DirectMode() = default;

DirectMode::~DirectMode() {
    if (d3dDevice_) {
        static_cast<ID3D11Device*>(d3dDevice_)->Release();
        d3dDevice_ = nullptr;
    }
    if (adapter_) {
        adapter_->Release();
        adapter_ = nullptr;
    }
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
    if (!loggedCreate_) {
        loggedCreate_ = true;
        char line[256];
        if (desc) {
            sprintf_s(line, "Direct Mode CreateSwapTextureSet pid=%u size=%ux%u format=%u samples=%u; unsupported skeleton returning no handles",
                      pid, desc->nWidth, desc->nHeight, desc->nFormat, desc->nSampleCount);
        } else {
            sprintf_s(line, "Direct Mode CreateSwapTextureSet pid=%u desc=null; unsupported skeleton returning no handles", pid);
        }
        vr::VRDriverLog()->Log(line);
    }
}

void DirectMode::DestroySwapTextureSet(vr::SharedTextureHandle_t) {
    LogOnce(loggedDestroy_, "Direct Mode DestroySwapTextureSet");
}

void DirectMode::DestroyAllSwapTextureSets(uint32_t) {
    LogOnce(loggedDestroyAll_, "Direct Mode DestroyAllSwapTextureSets");
}

void DirectMode::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t[2], uint32_t (*indices)[2]) {
    if (indices) {
        (*indices)[0] = 0;
        (*indices)[1] = 0;
    }
    LogOnce(loggedNext_, "Direct Mode GetNextSwapTextureSetIndex; no texture sets available");
}

void DirectMode::SubmitLayer(const SubmitLayerPerEye_t (&)[2]) {
    LogOnce(loggedSubmit_, "Direct Mode SubmitLayer");
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
