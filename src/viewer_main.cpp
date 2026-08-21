#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include "live_frame_ipc.h"
#include "third_person_gpu_ipc.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

HWND g_hwnd = nullptr;
IDXGIAdapter1* g_adapter = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psFlip = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11Texture2D* g_sharedTexture = nullptr;
IDXGIKeyedMutex* g_sharedMutex = nullptr;
ID3D11Texture2D* g_localTexture = nullptr;
ID3D11ShaderResourceView* g_localSrv = nullptr;
HANDLE g_mapping = nullptr;
bigscreen_live_frame_ipc::LiveFrameState* g_state = nullptr;
uint64_t g_generation = 0;
uint64_t g_handle = 0;
uint64_t g_lastSequence = 0;
uint64_t g_frames = 0;
uint64_t g_copies = 0;
uint64_t g_dropped = 0;
uint64_t g_opens = 0;
ULONGLONG g_started = 0;
ULONGLONG g_lastMetrics = 0;
bool g_waitingLogged = false;
uint32_t g_sourceWidth = 0;
uint32_t g_sourceHeight = 0;
enum class ViewSource { FirstPerson, ThirdPerson };
ViewSource g_viewSource = ViewSource::FirstPerson;
HANDLE g_thirdMapping = nullptr;
bigscreen_third_person_gpu::State* g_thirdState = nullptr;
ID3D11Texture2D* g_thirdShared = nullptr;
IDXGIKeyedMutex* g_thirdMutex = nullptr;
ID3D11Texture2D* g_thirdLocal = nullptr;
ID3D11ShaderResourceView* g_thirdSrv = nullptr;
uint64_t g_thirdGeneration = 0;
uint64_t g_thirdHandle = 0;
uint64_t g_thirdSequence = 0;
ULONGLONG g_thirdLastFrame = 0;
bool g_thirdUnavailable = false;

void Log(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    std::printf("%s\n", buffer);
    std::fflush(stdout);
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

void ReleaseSharedResources() {
    if (g_localSrv) g_localSrv->Release();
    if (g_localTexture) g_localTexture->Release();
    if (g_sharedMutex) g_sharedMutex->Release();
    if (g_sharedTexture) g_sharedTexture->Release();
    g_localSrv = nullptr;
    g_localTexture = nullptr;
    g_sharedMutex = nullptr;
    g_sharedTexture = nullptr;
    g_generation = 0;
    g_handle = 0;
    g_lastSequence = 0;
    g_sourceWidth = 0;
    g_sourceHeight = 0;
}

void ReleaseThirdPersonResources() {
    if (g_thirdSrv) g_thirdSrv->Release();
    if (g_thirdLocal) g_thirdLocal->Release();
    if (g_thirdMutex) g_thirdMutex->Release();
    if (g_thirdShared) g_thirdShared->Release();
    if (g_thirdState) UnmapViewOfFile(g_thirdState);
    if (g_thirdMapping) CloseHandle(g_thirdMapping);
    g_thirdSrv = nullptr; g_thirdLocal = nullptr; g_thirdMutex = nullptr; g_thirdShared = nullptr;
    g_thirdState = nullptr; g_thirdMapping = nullptr; g_thirdGeneration = 0; g_thirdHandle = 0; g_thirdSequence = 0; g_thirdLastFrame = 0;
}

void ReleaseAll() {
    ReleaseSharedResources();
    ReleaseThirdPersonResources();
    if (g_state) UnmapViewOfFile(g_state);
    if (g_mapping) CloseHandle(g_mapping);
    if (g_rtv) g_rtv->Release();
    if (g_sampler) g_sampler->Release();
    if (g_ps) g_ps->Release();
    if (g_psFlip) g_psFlip->Release();
    if (g_vs) g_vs->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    if (g_adapter) g_adapter->Release();
    g_state = nullptr;
    g_mapping = nullptr;
    g_rtv = nullptr;
    g_sampler = nullptr;
    g_ps = nullptr;
    g_psFlip = nullptr;
    g_vs = nullptr;
    g_swap = nullptr;
    g_context = nullptr;
    g_device = nullptr;
    g_adapter = nullptr;
}

bool OpenThirdPersonMapping() {
    if (g_thirdState) return true;
    g_thirdMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_third_person_gpu::kMappingName);
    if (!g_thirdMapping) return false;
    g_thirdState = static_cast<bigscreen_third_person_gpu::State*>(MapViewOfFile(g_thirdMapping, FILE_MAP_READ, 0, 0, sizeof(*g_thirdState)));
    if (!g_thirdState) { CloseHandle(g_thirdMapping); g_thirdMapping = nullptr; return false; }
    Log("Third-person GPU metadata mapping opened");
    return true;
}

bool OpenThirdPersonShared(const bigscreen_third_person_gpu::State& state) {
    if (g_thirdSrv) g_thirdSrv->Release();
    if (g_thirdLocal) g_thirdLocal->Release();
    if (g_thirdMutex) g_thirdMutex->Release();
    if (g_thirdShared) g_thirdShared->Release();
    g_thirdSrv = nullptr; g_thirdLocal = nullptr; g_thirdMutex = nullptr; g_thirdShared = nullptr;
    HRESULT hr = g_device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(state.sharedHandle)), IID_PPV_ARGS(&g_thirdShared));
    if (FAILED(hr)) return false;
    if (FAILED(g_thirdShared->QueryInterface(IID_PPV_ARGS(&g_thirdMutex)))) { ReleaseThirdPersonResources(); return false; }
    D3D11_TEXTURE2D_DESC desc{}; g_thirdShared->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC localDesc = desc; localDesc.MiscFlags = 0; localDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; localDesc.CPUAccessFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&localDesc, nullptr, &g_thirdLocal)) || FAILED(g_device->CreateShaderResourceView(g_thirdLocal, nullptr, &g_thirdSrv))) { ReleaseThirdPersonResources(); return false; }
    g_thirdGeneration = state.generation; g_thirdHandle = state.sharedHandle; g_sourceWidth = desc.Width; g_sourceHeight = desc.Height;
    Log("Third-person shared texture opened");
    return true;
}

bool CopyThirdPersonFrame() {
    if (!OpenThirdPersonMapping() || !g_thirdState) return false;
    const auto snapshot = *g_thirdState;
    if (snapshot.magic != bigscreen_third_person_gpu::kMagic || snapshot.version != bigscreen_third_person_gpu::kVersion || snapshot.valid == 0 || snapshot.sharedHandle == 0 || snapshot.frameSequence == 0) return false;
    if (!g_thirdLocal || g_thirdGeneration != snapshot.generation || g_thirdHandle != snapshot.sharedHandle) if (!OpenThirdPersonShared(snapshot)) return false;
    if (snapshot.frameSequence == static_cast<LONG64>(g_thirdSequence)) return true;
    if (!g_thirdMutex || FAILED(g_thirdMutex->AcquireSync(1, 0))) return false;
    g_context->CopyResource(g_thirdLocal, g_thirdShared); g_context->Flush(); g_thirdMutex->ReleaseSync(0);
    g_thirdSequence = static_cast<uint64_t>(snapshot.frameSequence); g_thirdLastFrame = GetTickCount64(); return true;
}

void SelectViewSource(ViewSource source) {
    g_viewSource = source;
    if (source == ViewSource::ThirdPerson) g_thirdUnavailable = false;
    if (source == ViewSource::FirstPerson) ReleaseThirdPersonResources();
}

bool OpenLiveMapping() {
    if (g_state) return true;
    g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_live_frame_ipc::kMappingName);
    if (!g_mapping) return false;
    g_state = static_cast<bigscreen_live_frame_ipc::LiveFrameState*>(MapViewOfFile(
        g_mapping, FILE_MAP_READ, 0, 0, sizeof(bigscreen_live_frame_ipc::LiveFrameState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    Log("Live frame metadata mapping opened");
    return true;
}

bool SelectNvidiaAdapter() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (candidate && SUCCEEDED(candidate->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && desc.VendorId == 0x10DE) {
            g_adapter = candidate;
            Log("Viewer adapter: %ls", desc.Description);
            break;
        }
        if (candidate) candidate->Release();
    }
    factory->Release();
    return g_adapter != nullptr;
}

bool InitD3D() {
    if (!SelectNvidiaAdapter()) return false;
    RECT rect{};
    GetClientRect(g_hwnd, &rect);
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 2;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = g_hwnd;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swapDesc.BufferDesc.Width = static_cast<UINT>(std::max<LONG>(1, rect.right));
    swapDesc.BufferDesc.Height = static_cast<UINT>(std::max<LONG>(1, rect.bottom));
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        g_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &swapDesc, &g_swap, &g_device, &level, &g_context);
    if (FAILED(hr)) {
        Log("D3D11CreateDeviceAndSwapChain failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv))) {
        if (backBuffer) backBuffer->Release();
        return false;
    }
    backBuffer->Release();
    const char* vertexCode =
        "struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
        "O main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};"
        "float2 u[3]={float2(0,1),float2(0,-1),float2(2,1)};O o;o.p=float4(p[id],0,1);o.uv=u[id];return o;}";
    const char* pixelCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.Sample(s,uv);}";
    const char* pixelFlipCode =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.Sample(s,float2(uv.x,1.0-uv.y));}";
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    hr = D3DCompile(vertexCode, std::strlen(vertexCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_vs);
    blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(pixelCode, std::strlen(pixelCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_ps);
    blob->Release();
    if (FAILED(hr)) return false;
    hr = D3DCompile(pixelFlipCode, std::strlen(pixelFlipCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) { if (errors) errors->Release(); return false; }
    hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_psFlip);
    blob->Release();
    if (FAILED(hr)) return false;
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    return SUCCEEDED(g_device->CreateSamplerState(&samplerDesc, &g_sampler));
}

void ResizeSwapChain(UINT width, UINT height) {
    if (!g_swap || width == 0 || height == 0) return;
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
    g_swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

bool OpenSharedFrame(const bigscreen_live_frame_ipc::LiveFrameState& state) {
    ReleaseSharedResources();
    HRESULT hr = g_device->OpenSharedResource(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(state.sharedHandle)),
        IID_PPV_ARGS(&g_sharedTexture));
    if (FAILED(hr)) {
        Log("OpenSharedResource failed: 0x%08X", static_cast<unsigned>(hr));
        ReleaseSharedResources();
        return false;
    }
    if (FAILED(g_sharedTexture->QueryInterface(IID_PPV_ARGS(&g_sharedMutex)))) {
        Log("Shared output has no keyed mutex");
        ReleaseSharedResources();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    g_sharedTexture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC localDesc = desc;
    localDesc.Usage = D3D11_USAGE_DEFAULT;
    localDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    localDesc.CPUAccessFlags = 0;
    localDesc.MiscFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&localDesc, nullptr, &g_localTexture)) ||
        FAILED(g_device->CreateShaderResourceView(g_localTexture, nullptr, &g_localSrv))) {
        Log("Viewer local texture creation failed");
        ReleaseSharedResources();
        return false;
    }
    g_generation = state.generation;
    g_handle = state.sharedHandle;
    g_sourceWidth = desc.Width;
    g_sourceHeight = desc.Height;
    ++g_opens;
    Log("Opened live texture generation=%llu size=%ux%u format=%u",
        static_cast<unsigned long long>(g_generation), g_sourceWidth, g_sourceHeight, desc.Format);
    return true;
}

bool CopyNewFrame() {
    if (!g_state) return false;
    const auto snapshot = *g_state;
    if (snapshot.magic != bigscreen_live_frame_ipc::kMagic || snapshot.version != bigscreen_live_frame_ipc::kVersion ||
        snapshot.valid == 0 || snapshot.sharedHandle == 0 || snapshot.frameSequence == 0) return false;
    if (!g_localTexture || g_generation != snapshot.generation || g_handle != snapshot.sharedHandle) {
        if (!OpenSharedFrame(snapshot)) return false;
    }
    if (snapshot.frameSequence == static_cast<LONG64>(g_lastSequence)) return false;
    if (!g_sharedMutex || FAILED(g_sharedMutex->AcquireSync(1, 0))) return false;
    g_context->CopyResource(g_localTexture, g_sharedTexture);
    g_context->Flush();
    g_sharedMutex->ReleaseSync(0);
    const uint64_t sequence = static_cast<uint64_t>(snapshot.frameSequence);
    if (g_lastSequence != 0 && sequence > g_lastSequence + 1) g_dropped += sequence - g_lastSequence - 1;
    g_lastSequence = sequence;
    ++g_copies;
    return true;
}

void Render(ID3D11ShaderResourceView* source, bool flipVertical) {
    RECT rect{};
    GetClientRect(g_hwnd, &rect);
    const float windowWidth = static_cast<float>(std::max<LONG>(1, rect.right));
    const float windowHeight = static_cast<float>(std::max<LONG>(1, rect.bottom));
    const float sourceAspect = g_sourceHeight ? static_cast<float>(g_sourceWidth) / g_sourceHeight : 16.0f / 9.0f;
    const float windowAspect = windowWidth / windowHeight;
    D3D11_VIEWPORT viewport{};
    if (windowAspect > sourceAspect) {
        viewport.Height = windowHeight;
        viewport.Width = windowHeight * sourceAspect;
        viewport.TopLeftX = (windowWidth - viewport.Width) * 0.5f;
    } else {
        viewport.Width = windowWidth;
        viewport.Height = windowWidth / sourceAspect;
        viewport.TopLeftY = (windowHeight - viewport.Height) * 0.5f;
    }
    const float clear[4] = {0.04f, 0.04f, 0.05f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    if (source) {
        g_context->RSSetViewports(1, &viewport);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_vs, nullptr, 0);
        g_context->PSSetShader(flipVertical ? g_psFlip : g_ps, nullptr, 0);
        g_context->PSSetSamplers(0, 1, &g_sampler);
        g_context->PSSetShaderResources(0, 1, &source);
        g_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_context->PSSetShaderResources(0, 1, &nullSrv);
    }
    g_swap->Present(1, 0);
    ++g_frames;
}

void UpdateMetrics() {
    const ULONGLONG now = GetTickCount64();
    if (!g_lastMetrics) g_lastMetrics = now;
    if (now - g_lastMetrics < 1000) return;
    const double elapsed = static_cast<double>(now - g_started) / 1000.0;
    const double fps = elapsed > 0.0 ? static_cast<double>(g_frames) / elapsed : 0.0;
    Log("Viewer metrics: fps=%.1f sourceSeq=%llu copies=%llu dropped=%llu opens=%llu cpuCopy=GPU-only",
        fps, static_cast<unsigned long long>(g_lastSequence), static_cast<unsigned long long>(g_copies),
        static_cast<unsigned long long>(g_dropped), static_cast<unsigned long long>(g_opens));
    g_lastMetrics = now;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KEYDOWN && (wParam == '1' || wParam == '3')) {
        SelectViewSource(wParam == '3' ? ViewSource::ThirdPerson : ViewSource::FirstPerson);
        return 0;
    }
    if (message == WM_SIZE) ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
    if (message == WM_NCHITTEST) {
        // The viewer is display-only, so let the rendered client area act as
        // a drag surface even when the title bar is outside the visible crop.
        const LRESULT hit = DefWindowProc(hwnd, message, wParam, lParam);
        return hit == HTCLIENT ? HTCAPTION : hit;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WndProc;
    windowClass.lpszClassName = "BigscreenDesktopViewer";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&windowClass);
    g_hwnd = CreateWindowA(windowClass.lpszClassName, "Bigscreen Desktop Live Viewer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1280, 720,
                           nullptr, nullptr, instance, nullptr);
    if (!g_hwnd || !InitD3D()) {
        ReleaseAll();
        return 2;
    }
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetWindowPos(g_hwnd, nullptr, 100, 100, 1280, 720,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_started = GetTickCount64();
    Log("BigscreenDesktopViewer GPU live core starting");
    MSG message{};
    while (message.message != WM_QUIT) {
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        ID3D11ShaderResourceView* source = nullptr;
        if (g_viewSource == ViewSource::ThirdPerson) {
            const bool copied = CopyThirdPersonFrame();
            if (!copied || !g_thirdSrv || (g_thirdLastFrame && GetTickCount64() - g_thirdLastFrame > 1500)) {
                g_thirdUnavailable = true;
                SelectViewSource(ViewSource::FirstPerson);
            } else {
                source = g_thirdSrv;
            }
        }
        if (g_viewSource == ViewSource::FirstPerson) {
            OpenLiveMapping();
            if (g_state) CopyNewFrame();
            source = g_localSrv;
        }
        char title[256]{};
        const char* view = g_viewSource == ViewSource::ThirdPerson ? "THIRD PERSON" : "FIRST PERSON";
        const char* status = g_thirdUnavailable ? " | Third-person camera unavailable" : "";
        sprintf_s(title, "Bigscreen Desktop Live Viewer | View: %s | 1=FIRST PERSON  3=THIRD PERSON%s", view, status);
        SetWindowTextA(g_hwnd, title);
        Render(source, g_viewSource == ViewSource::ThirdPerson);
        UpdateMetrics();
    }
    ReleaseAll();
    return 0;
}
