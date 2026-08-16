#include "direct_mode.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include "live_frame_ipc.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
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
    ReleaseLiveOutput();
    ReleaseSyncTexture();
    if (diagnosticStaging_) {
        diagnosticStaging_->Release();
        diagnosticStaging_ = nullptr;
    }
    if (d3dContext_) {
        d3dContext_->Release();
        d3dContext_ = nullptr;
    }
    if (d3dDevice_) {
        d3dDevice_->Release();
        d3dDevice_ = nullptr;
    }
    if (adapter_) {
        adapter_->Release();
        adapter_ = nullptr;
    }
    if (liveState_) {
        UnmapViewOfFile(liveState_);
        liveState_ = nullptr;
    }
    if (liveMapping_) {
        CloseHandle(liveMapping_);
        liveMapping_ = nullptr;
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
        const HRESULT hr = D3D11CreateDevice(
            adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, &level, &d3dContext_);
        if (FAILED(hr)) {
            if (d3dContext_) d3dContext_->Release();
            d3dContext_ = nullptr;
            adapter_->Release();
            adapter_ = nullptr;
            adapterLuid_ = 0;
            return false;
        }
        d3dDevice_ = device;
        EnsureLiveIpc();
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

namespace {

std::wstring DiagnosticDirectory() {
    wchar_t tempPath[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    std::filesystem::path directory = (length > 0 && length < std::size(tempPath))
        ? std::filesystem::path(tempPath)
        : std::filesystem::path(L".");
    directory /= L"BigscreenDesktopBridge";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory.wstring();
}

std::wstring DiagnosticRequestPath() {
    return (std::filesystem::path(DiagnosticDirectory()) / L"direct_mode_capture.request").wstring();
}

std::wstring DiagnosticFramePath() {
    return (std::filesystem::path(DiagnosticDirectory()) / L"direct_mode_left_eye.raw").wstring();
}

#pragma pack(push, 1)
struct RawFrameHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t rowPitch;
    uint32_t format;
    uint32_t byteCount;
    uint64_t sequence;
};
#pragma pack(pop)

constexpr uint32_t kRawFrameMagic = 0x52464442; // "BDFR"

}

bool DirectMode::EnsureDiagnosticStaging(TextureSet* set) {
    if (!set || !d3dDevice_ || !d3dContext_ || set->sampleCount != 1) return false;
    if (diagnosticStaging_ && diagnosticWidth_ == set->width && diagnosticHeight_ == set->height &&
        diagnosticFormat_ == set->format && diagnosticSamples_ == set->sampleCount) return true;
    if (diagnosticStaging_) {
        diagnosticStaging_->Release();
        diagnosticStaging_ = nullptr;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = set->width;
    desc.Height = set->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(set->format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d3dDevice_->CreateTexture2D(&desc, nullptr, &diagnosticStaging_))) {
        diagnosticStaging_ = nullptr;
        return false;
    }
    diagnosticWidth_ = set->width;
    diagnosticHeight_ = set->height;
    diagnosticFormat_ = set->format;
    diagnosticSamples_ = set->sampleCount;
    return true;
}

bool DirectMode::CaptureRequested() const {
    return GetFileAttributesW(DiagnosticRequestPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirectMode::WriteRawDiagnosticFrame(const TextureSet* set, uint32_t textureIndex) {
    if (!set || textureIndex >= 3 || !set->textures[textureIndex] ||
        !EnsureDiagnosticStaging(const_cast<TextureSet*>(set))) {
        LogOnce(loggedCapture_, "Direct Mode raw capture failed: staging resource unavailable");
        return false;
    }
    d3dContext_->CopyResource(diagnosticStaging_, set->textures[textureIndex]);
    d3dContext_->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3dContext_->Map(diagnosticStaging_, 0, D3D11_MAP_READ, 0, &mapped))) {
        LogOnce(loggedCapture_, "Direct Mode raw capture failed: staging map failed");
        return false;
    }

    const uint32_t byteCount = mapped.RowPitch * diagnosticHeight_;
    RawFrameHeader header{
        kRawFrameMagic, 1, diagnosticWidth_, diagnosticHeight_, mapped.RowPitch,
        diagnosticFormat_, byteCount, ++diagnosticSequence_};
    const std::wstring finalPath = DiagnosticFramePath();
    const std::wstring temporaryPath = finalPath + L".tmp";
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool success = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        success = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header);
        for (uint32_t row = 0; success && row < diagnosticHeight_; ++row) {
            const BYTE* source = static_cast<const BYTE*>(mapped.pData) +
                                 static_cast<size_t>(row) * mapped.RowPitch;
            success = WriteFile(file, source, mapped.RowPitch, &written, nullptr) && written == mapped.RowPitch;
        }
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    if (success) {
        DeleteFileW(finalPath.c_str());
        success = MoveFileW(temporaryPath.c_str(), finalPath.c_str()) != FALSE;
    } else {
        DeleteFileW(temporaryPath.c_str());
    }
    d3dContext_->Unmap(diagnosticStaging_, 0);

    char line[512];
    sprintf_s(line, "Direct Mode raw left-eye capture %s size=%ux%u format=%u rowPitch=%u bytes=%u sequence=%llu path=%ls",
              success ? "saved" : "failed", diagnosticWidth_, diagnosticHeight_, diagnosticFormat_,
              mapped.RowPitch, byteCount, static_cast<unsigned long long>(header.sequence), finalPath.c_str());
    vr::VRDriverLog()->Log(line);
    return success;
}

void DirectMode::ReleaseSyncTexture() {
    if (syncMutex_) {
        syncMutex_->Release();
        syncMutex_ = nullptr;
    }
    if (syncTexture_) {
        syncTexture_->Release();
        syncTexture_ = nullptr;
    }
    syncHandle_ = 0;
}

bool DirectMode::EnsureLiveIpc() {
    if (liveState_) return true;
    liveMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(sizeof(bigscreen_live_frame_ipc::LiveFrameState)),
                                      bigscreen_live_frame_ipc::kMappingName);
    if (!liveMapping_) return false;
    liveState_ = static_cast<bigscreen_live_frame_ipc::LiveFrameState*>(MapViewOfFile(
        liveMapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bigscreen_live_frame_ipc::LiveFrameState)));
    if (!liveState_) {
        CloseHandle(liveMapping_);
        liveMapping_ = nullptr;
        return false;
    }
    *liveState_ = bigscreen_live_frame_ipc::LiveFrameState{};
    if (!loggedLiveIpc_) {
        loggedLiveIpc_ = true;
        vr::VRDriverLog()->Log("Direct Mode live GPU IPC mapping ready");
    }
    return true;
}

void DirectMode::ReleaseLiveOutput() {
    if (liveState_) InterlockedExchange(&liveState_->valid, 0);
    if (liveMutex_) {
        liveMutex_->Release();
        liveMutex_ = nullptr;
    }
    if (liveOutput_) {
        liveOutput_->Release();
        liveOutput_ = nullptr;
    }
    liveWidth_ = 0;
    liveHeight_ = 0;
    liveFormat_ = 0;
}

bool DirectMode::EnsureLiveOutput(TextureSet* set) {
    if (!set || !d3dDevice_ || !EnsureLiveIpc()) return false;
    if (liveOutput_ && liveWidth_ == set->width && liveHeight_ == set->height && liveFormat_ == set->format) {
        return true;
    }

    ReleaseLiveOutput();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = set->width;
    desc.Height = set->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(set->format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(d3dDevice_->CreateTexture2D(&desc, nullptr, &liveOutput_))) {
        liveOutput_ = nullptr;
        return false;
    }

    IDXGIResource* resource = nullptr;
    HANDLE sharedHandle = nullptr;
    HRESULT hr = liveOutput_->QueryInterface(IID_PPV_ARGS(&resource));
    if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&sharedHandle);
    if (resource) resource->Release();
    if (FAILED(hr) || !sharedHandle || FAILED(liveOutput_->QueryInterface(IID_PPV_ARGS(&liveMutex_)))) {
        ReleaseLiveOutput();
        return false;
    }

    liveWidth_ = set->width;
    liveHeight_ = set->height;
    liveFormat_ = set->format;
    ++liveGeneration_;
    liveFrameSequence_ = 0;
    liveState_->width = liveWidth_;
    liveState_->height = liveHeight_;
    liveState_->format = liveFormat_;
    liveState_->sharedHandle = reinterpret_cast<uint64_t>(sharedHandle);
    liveState_->adapterLuid = adapterLuid_;
    liveState_->generation = liveGeneration_;
    InterlockedExchange64(&liveState_->frameSequence, 0);
    InterlockedExchange(&liveState_->valid, 1);

    if (!loggedLiveOutput_) {
        loggedLiveOutput_ = true;
        char line[384];
        sprintf_s(line, "Direct Mode live output created size=%ux%u format=%u handle=0x%llX generation=%llu",
                  liveWidth_, liveHeight_, liveFormat_, static_cast<unsigned long long>(liveState_->sharedHandle),
                  static_cast<unsigned long long>(liveGeneration_));
        vr::VRDriverLog()->Log(line);
    }
    return true;
}

bool DirectMode::PublishLiveFrame(TextureSet* set, uint32_t textureIndex) {
    if (!EnsureLiveOutput(set) || !liveOutput_ || !liveMutex_ || textureIndex >= 3 || !set->textures[textureIndex]) {
        return false;
    }
    const HRESULT acquire = liveMutex_->AcquireSync(0, 0);
    if (FAILED(acquire)) {
        if (!loggedLiveDrop_) {
            loggedLiveDrop_ = true;
            vr::VRDriverLog()->Log("Direct Mode live output waiting for viewer");
        }
        return false;
    }
    d3dContext_->CopyResource(liveOutput_, set->textures[textureIndex]);
    d3dContext_->Flush();
    ++liveFrameSequence_;
    InterlockedExchange64(&liveState_->frameSequence, static_cast<LONG64>(liveFrameSequence_));
    liveMutex_->ReleaseSync(1);
    return true;
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

void DirectMode::Present(vr::SharedTextureHandle_t syncTexture) {
    SubmitDiagnostic left;
    TextureSet* liveSet = nullptr;
    uint32_t liveTextureIndex = 0;
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        left = lastSubmit_;
        const auto found = handleIndex_.find(left.handles[0]);
        if (found != handleIndex_.end()) {
            liveSet = found->second;
            liveTextureIndex = left.indices[0];
        }
    }
    if (liveSet) {
        if (syncHandle_ != syncTexture) {
            ReleaseSyncTexture();
            syncHandle_ = syncTexture;
            if (syncHandle_ && FAILED(d3dDevice_->OpenSharedResource(
                    reinterpret_cast<HANDLE>(static_cast<uintptr_t>(syncHandle_)),
                    IID_PPV_ARGS(&syncTexture_)))) {
                syncTexture_ = nullptr;
            }
            if (syncTexture_) syncTexture_->QueryInterface(IID_PPV_ARGS(&syncMutex_));
        }
        if (!syncTexture_ || !syncMutex_) {
            LogOnce(loggedSync_, "Direct Mode live output failed: sync texture has no keyed mutex");
        } else if (SUCCEEDED(syncMutex_->AcquireSync(0, 100))) {
            if (!loggedSync_) {
                loggedSync_ = true;
                vr::VRDriverLog()->Log("Direct Mode live output synchronization acquired");
            }
            PublishLiveFrame(liveSet, liveTextureIndex);
            if (CaptureRequested()) {
                DeleteFileW(DiagnosticRequestPath().c_str());
                WriteRawDiagnosticFrame(liveSet, liveTextureIndex);
            }
            syncMutex_->ReleaseSync(0);
        } else {
            LogOnce(loggedSync_, "Direct Mode live output failed: AcquireSync failed");
        }
    }
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
