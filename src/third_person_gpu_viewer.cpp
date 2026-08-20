#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include "third_person_gpu_ipc.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
HWND g_hwnd = nullptr;
HANDLE g_mapping = nullptr;
bigscreen_third_person_gpu::State* g_state = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_shared = nullptr;
IDXGIKeyedMutex* g_mutex = nullptr;
ID3D11Texture2D* g_local = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
uint64_t g_generation = 0;
uint64_t g_sequence = 0;
uint64_t g_copies = 0;
uint64_t g_dropped = 0;
ULONGLONG g_started = 0;
ULONGLONG g_lastLog = 0;

void Log(const char* text) { std::printf("%s\n", text); std::fflush(stdout); OutputDebugStringA(text); OutputDebugStringA("\n"); }

void ReleaseTexture() {
    if (g_srv) g_srv->Release(); if (g_local) g_local->Release();
    if (g_mutex) g_mutex->Release(); if (g_shared) g_shared->Release();
    g_srv = nullptr; g_local = nullptr; g_mutex = nullptr; g_shared = nullptr;
    g_generation = 0; g_sequence = 0;
}

void Cleanup() {
    ReleaseTexture();
    if (g_state) UnmapViewOfFile(g_state);
    if (g_mapping) CloseHandle(g_mapping);
    if (g_rtv) g_rtv->Release(); if (g_swap) g_swap->Release();
    if (g_sampler) g_sampler->Release(); if (g_ps) g_ps->Release(); if (g_vs) g_vs->Release();
    if (g_context) g_context->Release(); if (g_device) g_device->Release();
    g_state = nullptr; g_mapping = nullptr; g_rtv = nullptr; g_swap = nullptr;
    g_context = nullptr; g_device = nullptr;
}

bool InitD3D() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId == 0x10DE) break;
        adapter->Release(); adapter = nullptr;
    }
    if (!adapter) { factory->Release(); return false; }
    DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
    char name[256]{}; WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    char log[512]{}; sprintf_s(log, "Third-person viewer adapter: %s", name); Log(log);
    RECT r{}; GetClientRect(g_hwnd, &r);
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_hwnd; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.BufferDesc.Width = std::max<LONG>(1, r.right); sd.BufferDesc.Height = std::max<LONG>(1, r.bottom);
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &fl, &g_context);
    adapter->Release(); factory->Release();
    if (FAILED(hr)) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || FAILED(g_device->CreateRenderTargetView(back, nullptr, &g_rtv))) { if (back) back->Release(); return false; }
    back->Release();
    const char* vsCode = "struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};O main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};float2 u[3]={float2(0,0),float2(0,2),float2(2,0)};O o;o.p=float4(p[id],0,1);o.uv=u[id];return o;}";
    const char* psCode = "Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.Sample(s,uv);}";
    ID3DBlob* blob = nullptr; ID3DBlob* errors = nullptr;
    hr = D3DCompile(vsCode, strlen(vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_vs); blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(psCode, strlen(psCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_ps); blob->Release();
    if (FAILED(hr)) return false;
    D3D11_SAMPLER_DESC samp{}; samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    return SUCCEEDED(g_device->CreateSamplerState(&samp, &g_sampler));
}

bool OpenMapping() {
    if (g_state) return true;
    g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_third_person_gpu::kMappingName);
    if (!g_mapping) return false;
    g_state = static_cast<bigscreen_third_person_gpu::State*>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, sizeof(*g_state)));
    if (!g_state) { CloseHandle(g_mapping); g_mapping = nullptr; return false; }
    Log("Third-person GPU metadata mapping opened"); return true;
}

bool OpenShared(const bigscreen_third_person_gpu::State& s) {
    ReleaseTexture();
    HRESULT hr = g_device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(s.sharedHandle)), IID_PPV_ARGS(&g_shared));
    if (FAILED(hr)) return false;
    if (FAILED(g_shared->QueryInterface(IID_PPV_ARGS(&g_mutex)))) { ReleaseTexture(); return false; }
    D3D11_TEXTURE2D_DESC d{}; g_shared->GetDesc(&d);
    D3D11_TEXTURE2D_DESC local = d; local.MiscFlags = 0; local.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&local, nullptr, &g_local)) || FAILED(g_device->CreateShaderResourceView(g_local, nullptr, &g_srv))) { ReleaseTexture(); return false; }
    g_generation = s.generation;
    char log[256]{}; sprintf_s(log, "Opened third-person shared texture %ux%u format=%u", d.Width, d.Height, d.Format); Log(log);
    return true;
}

bool CopyFrame() {
    if (!g_state || g_state->magic != bigscreen_third_person_gpu::kMagic || g_state->version != bigscreen_third_person_gpu::kVersion || !g_state->valid || !g_state->sharedHandle || !g_state->frameSequence) return false;
    auto s = *g_state;
    if (!g_local || g_generation != s.generation) if (!OpenShared(s)) return false;
    if (s.frameSequence == static_cast<LONG64>(g_sequence)) return false;
    if (!g_mutex || FAILED(g_mutex->AcquireSync(1, 0))) return false;
    g_context->CopyResource(g_local, g_shared); g_context->Flush(); g_mutex->ReleaseSync(0);
    if (g_sequence && s.frameSequence > static_cast<LONG64>(g_sequence + 1)) g_dropped += s.frameSequence - g_sequence - 1;
    g_sequence = static_cast<uint64_t>(s.frameSequence); ++g_copies; return true;
}

void Render() {
    RECT r{}; GetClientRect(g_hwnd, &r); const float ww = std::max<LONG>(1, r.right), wh = std::max<LONG>(1, r.bottom);
    const float sourceAspect = 1280.0f / 720.0f, aspect = ww / wh; D3D11_VIEWPORT vp{};
    if (aspect > sourceAspect) { vp.Height = wh; vp.Width = wh * sourceAspect; vp.TopLeftX = (ww - vp.Width) * .5f; }
    else { vp.Width = ww; vp.Height = ww / sourceAspect; vp.TopLeftY = (wh - vp.Height) * .5f; }
    const float clear[4] = {.03f,.03f,.04f,1}; g_context->OMSetRenderTargets(1, &g_rtv, nullptr); g_context->ClearRenderTargetView(g_rtv, clear);
    if (g_srv) {
        g_context->RSSetViewports(1, &vp); g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_vs, nullptr, 0); g_context->PSSetShader(g_ps, nullptr, 0); g_context->PSSetSamplers(0, 1, &g_sampler);
        g_context->PSSetShaderResources(0, 1, &g_srv); g_context->Draw(3, 0); ID3D11ShaderResourceView* nullSrv = nullptr; g_context->PSSetShaderResources(0, 1, &nullSrv);
    }
    if (g_swap) g_swap->Present(1, 0);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_DESTROY) { PostQuitMessage(0); return 0; } return DefWindowProc(h,m,w,l); }
}

int RunViewer() {
    WNDCLASSA wc{}; wc.hInstance = GetModuleHandleA(nullptr); wc.lpfnWndProc = WndProc; wc.lpszClassName = "BigscreenThirdPersonGPUViewer"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassA(&wc);
    g_hwnd = CreateWindowA(wc.lpszClassName, "Bigscreen NOVR Third-Person GPU Viewer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 140, 140, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd || !InitD3D()) { Cleanup(); return 2; }
    g_started = GetTickCount64(); ShowWindow(g_hwnd, SW_SHOWNORMAL);
    Log("Third-person GPU viewer waiting for CaptureCamera export");
    MSG msg{}; while (msg.message != WM_QUIT) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        OpenMapping(); CopyFrame(); Render();
        char title[256]{};
        sprintf_s(title, "Bigscreen NOVR Third-Person GPU Viewer | map=%s shared=%s copies=%llu seq=%llu",
                  g_state ? "yes" : "no", g_shared ? "yes" : "no",
                  (unsigned long long)g_copies, (unsigned long long)g_sequence);
        SetWindowTextA(g_hwnd, title);
        const auto now = GetTickCount64(); if (now - g_lastLog > 1000) { char b[256]{}; sprintf_s(b, "Third-person GPU metrics: copies=%llu dropped=%llu", (unsigned long long)g_copies, (unsigned long long)g_dropped); Log(b); g_lastLog = now; }
    }
    Cleanup(); return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return RunViewer(); }
