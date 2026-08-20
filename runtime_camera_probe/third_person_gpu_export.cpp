#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "../src/third_person_gpu_ipc.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {
ID3D11Texture2D* g_source = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11Texture2D* g_export = nullptr;
IDXGIKeyedMutex* g_mutex = nullptr;
HANDLE g_mapping = nullptr;
bigscreen_third_person_gpu::State* g_state = nullptr;
uint64_t g_generation = 0;

void WriteReport(const wchar_t* path, const char* source, const char* destination, const char* device) {
    if (!path) return;
    FILE* f = nullptr; _wfopen_s(&f, path, L"w, ccs=UTF-8"); if (!f) return;
    fwprintf(f, L"Source=%S\nDestination=%S\nUnityDevice=%S\n", source, destination, device);
    fclose(f);
}

void Cleanup() {
    if (g_state) { g_state->valid = 0; UnmapViewOfFile(g_state); }
    if (g_mapping) CloseHandle(g_mapping);
    if (g_mutex) g_mutex->Release(); if (g_export) g_export->Release();
    if (g_context) g_context->Release(); if (g_device) g_device->Release(); if (g_source) g_source->Release();
    g_state = nullptr; g_mapping = nullptr; g_mutex = nullptr; g_export = nullptr; g_context = nullptr; g_device = nullptr; g_source = nullptr; g_generation = 0;
}
}

extern "C" __declspec(dllexport) int __cdecl InitThirdPersonGpu(void* nativePtr, const wchar_t* reportPath) {
    Cleanup();
    if (!nativePtr) return 1;
    IUnknown* unknown = reinterpret_cast<IUnknown*>(nativePtr);
    if (FAILED(unknown->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&g_source)))) { Cleanup(); return 2; }
    g_source->GetDevice(&g_device);
    if (!g_device) { Cleanup(); return 3; }
    g_device->GetImmediateContext(&g_context);
    D3D11_TEXTURE2D_DESC src{}; g_source->GetDesc(&src);
    D3D11_TEXTURE2D_DESC dst = src;
    dst.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    dst.CPUAccessFlags = 0;
    dst.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(g_device->CreateTexture2D(&dst, nullptr, &g_export))) { Cleanup(); return 4; }
    if (FAILED(g_export->QueryInterface(IID_PPV_ARGS(&g_mutex)))) { Cleanup(); return 5; }
    IDXGIResource* resource = nullptr; if (FAILED(g_export->QueryInterface(IID_PPV_ARGS(&resource)))) { Cleanup(); return 6; }
    HANDLE shared = nullptr; HRESULT hrHandle = resource->GetSharedHandle(&shared); resource->Release();
    if (FAILED(hrHandle) || !shared) { Cleanup(); return 7; }
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bigscreen_third_person_gpu::State), bigscreen_third_person_gpu::kMappingName);
    if (!g_mapping) { Cleanup(); return 8; }
    g_state = static_cast<bigscreen_third_person_gpu::State*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bigscreen_third_person_gpu::State)));
    if (!g_state) { Cleanup(); return 9; }
    DXGI_ADAPTER_DESC ad{}; uint64_t luid = 0; IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) { IDXGIAdapter* adapter = nullptr; if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) { adapter->GetDesc(&ad); std::memcpy(&luid, &ad.AdapterLuid, sizeof(luid)); adapter->Release(); } dxgiDevice->Release(); }
    *g_state = {}; g_state->magic = bigscreen_third_person_gpu::kMagic; g_state->version = bigscreen_third_person_gpu::kVersion; g_state->width = dst.Width; g_state->height = dst.Height; g_state->format = static_cast<uint32_t>(dst.Format); g_state->sharedHandle = reinterpret_cast<uint64_t>(shared); g_state->adapterLuid = luid; g_generation = GetTickCount64(); g_state->generation = g_generation; g_state->valid = 1;
    ID3D11Device1* device1 = nullptr; ID3D11DeviceContext1* context1 = nullptr; IDXGIResource1* resource1 = nullptr;
    const bool hasDevice1 = SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&device1))); const bool hasContext1 = g_context && SUCCEEDED(g_context->QueryInterface(IID_PPV_ARGS(&context1))); const bool hasResource1 = SUCCEEDED(g_export->QueryInterface(IID_PPV_ARGS(&resource1)));
    if (device1) device1->Release(); if (context1) context1->Release(); if (resource1) resource1->Release();
    char s[256]{}, d[256]{}, dev[256]{}; sprintf_s(s, "source=%ux%u format=%u misc=0x%X", src.Width, src.Height, src.Format, src.MiscFlags); sprintf_s(d, "destination=%ux%u format=%u misc=0x%X keyedMutex=yes sharedHandle=%p", dst.Width, dst.Height, dst.Format, dst.MiscFlags, shared); sprintf_s(dev, "device1=%s context1=%s resource1=%s adapterLuid=0x%llX", hasDevice1?"yes":"no", hasContext1?"yes":"no", hasResource1?"yes":"no", (unsigned long long)luid); WriteReport(reportPath, s, d, dev);
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl CopyThirdPersonGpu(void* nativePtr) {
    if (!g_export || !g_mutex || !g_context || !nativePtr) return 1;
    ID3D11Texture2D* source = nullptr; if (FAILED(reinterpret_cast<IUnknown*>(nativePtr)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source)))) return 2;
    HRESULT hr = g_mutex->AcquireSync(0, 0); if (SUCCEEDED(hr)) { g_context->CopyResource(g_export, source); g_context->Flush(); g_mutex->ReleaseSync(1); if (g_state) { ++g_state->frameSequence; g_state->lastCopyMicros = GetTickCount64() * 1000ull; } }
    source->Release(); return SUCCEEDED(hr) ? 0 : 3;
}

extern "C" __declspec(dllexport) int __cdecl ShutdownThirdPersonGpu() { Cleanup(); return 0; }
