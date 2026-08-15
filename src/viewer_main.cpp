#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <openvr.h>
#include "virtual_display_ipc.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {
HWND g_hwnd = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11Texture2D* g_virtualTexture = nullptr;
ID3D11ShaderResourceView* g_virtualSrv = nullptr;
HANDLE g_virtualMapping = nullptr;
void* g_virtualMapped = nullptr;
LONG g_virtualSequence = 0;
vr::IVRSystem* g_vr = nullptr;
vr::IVRCompositor* g_compositor = nullptr;
bool g_rightEye = false;
unsigned long long g_frames = 0;
ULONGLONG g_started = 0;

void Log(const char* fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap);
    std::fprintf(stdout, "%s\n", buf); std::fflush(stdout);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
}

void ReleaseAll() {
    if (g_virtualSrv) g_virtualSrv->Release(); if (g_virtualTexture) g_virtualTexture->Release();
    if (g_sampler) g_sampler->Release(); if (g_ps) g_ps->Release(); if (g_vs) g_vs->Release();
    if (g_rtv) g_rtv->Release(); if (g_swap) g_swap->Release();
    if (g_context) g_context->Release(); if (g_device) g_device->Release();
    g_sampler=nullptr; g_ps=nullptr; g_vs=nullptr; g_rtv=nullptr; g_swap=nullptr; g_context=nullptr; g_device=nullptr;
    if (g_virtualMapped) UnmapViewOfFile(g_virtualMapped); g_virtualMapped=nullptr;
    if (g_virtualMapping) CloseHandle(g_virtualMapping); g_virtualMapping=nullptr;
    if (g_vr) { vr::VR_Shutdown(); g_vr=nullptr; g_compositor=nullptr; }
}

bool RenderSource(ID3D11ShaderResourceView* srv) {
    if (!srv) return false;
    const float clear[4]={0,0,0,1}; g_context->OMSetRenderTargets(1,&g_rtv,nullptr); g_context->ClearRenderTargetView(g_rtv,clear);
    g_context->VSSetShader(g_vs,nullptr,0); g_context->PSSetShader(g_ps,nullptr,0); g_context->PSSetSamplers(0,1,&g_sampler); g_context->PSSetShaderResources(0,1,&srv);
    D3D11_VIEWPORT vp{}; RECT r{}; GetClientRect(g_hwnd,&r); vp.Width=(float)r.right; vp.Height=(float)r.bottom; vp.MaxDepth=1; g_context->RSSetViewports(1,&vp); g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g_context->Draw(3,0);
    ID3D11ShaderResourceView* nullSrv=nullptr; g_context->PSSetShaderResources(0,1,&nullSrv); g_swap->Present(1,0); ++g_frames;
    return true;
}

bool RenderVirtualDisplay() {
    if (!g_virtualMapped) return false;
    auto* header = static_cast<bigscreen_virtual_display_ipc::FrameHeader*>(g_virtualMapped);
    const LONG sequence = InterlockedCompareExchange(&header->sequence, 0, 0);
    if (header->magic != bigscreen_virtual_display_ipc::kMagic || sequence == 0 || sequence == g_virtualSequence) return false;
    if (!header->width || !header->height || header->width > bigscreen_virtual_display_ipc::kMaxWidth || header->height > bigscreen_virtual_display_ipc::kMaxHeight) return false;
    bool recreate = !g_virtualTexture;
    if (!recreate) { D3D11_TEXTURE2D_DESC d{}; g_virtualTexture->GetDesc(&d); recreate = d.Width != header->width || d.Height != header->height; }
    if (recreate) {
        if (g_virtualSrv) g_virtualSrv->Release(); if (g_virtualTexture) g_virtualTexture->Release(); g_virtualSrv=nullptr; g_virtualTexture=nullptr;
        D3D11_TEXTURE2D_DESC d{}; d.Width=header->width; d.Height=header->height; d.MipLevels=1; d.ArraySize=1; d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count=1; d.Usage=D3D11_USAGE_DYNAMIC; d.BindFlags=D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_device->CreateTexture2D(&d,nullptr,&g_virtualTexture)) || FAILED(g_device->CreateShaderResourceView(g_virtualTexture,nullptr,&g_virtualSrv))) return false;
        Log("Virtual Display frame source: %ux%u format=%u", header->width, header->height, header->format);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_virtualTexture,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return false;
    const auto* pixels = reinterpret_cast<const unsigned char*>(header + 1);
    for (uint32_t y=0; y<header->height; ++y) std::memcpy(static_cast<unsigned char*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch, pixels+static_cast<size_t>(y)*header->stride, static_cast<size_t>(header->width)*4);
    g_context->Unmap(g_virtualTexture,0); g_virtualSequence=sequence;
    return RenderSource(g_virtualSrv);
}

bool InitD3D() {
    RECT r{}; GetClientRect(g_hwnd, &r);
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=g_hwnd; sd.SampleDesc.Count=1;
    sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD; sd.BufferDesc.Width=r.right; sd.BufferDesc.Height=r.bottom;
    UINT flags=0; D3D_FEATURE_LEVEL level{};
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags,nullptr,0,
        D3D11_SDK_VERSION,&sd,&g_swap,&g_device,&level,&g_context);
    if (FAILED(hr)) { Log("D3D11CreateDeviceAndSwapChain failed: 0x%08X", (unsigned)hr); return false; }
    ID3D11Texture2D* back=nullptr; hr=g_swap->GetBuffer(0,IID_PPV_ARGS(&back));
    if (FAILED(hr) || FAILED(g_device->CreateRenderTargetView(back,nullptr,&g_rtv))) { if(back) back->Release(); Log("Backbuffer setup failed: 0x%08X", (unsigned)hr); return false; }
    back->Release();
    const char* vsCode="struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;}; O main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};float2 u[3]={float2(0,1),float2(0,-1),float2(2,1)};O o;o.p=float4(p[id],0,1);o.uv=u[id];return o;}";
    const char* psCode="Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.Sample(s,uv);}";
    ID3DBlob* b=nullptr; ID3DBlob* e=nullptr;
    hr=D3DCompile(vsCode,strlen(vsCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&b,&e);
    if (FAILED(hr)) { Log("Vertex shader compile failed"); if(e)e->Release(); return false; }
    hr=g_device->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&g_vs); b->Release(); if(FAILED(hr)) return false;
    hr=D3DCompile(psCode,strlen(psCode),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&b,&e);
    if (FAILED(hr)) { Log("Pixel shader compile failed"); if(e)e->Release(); return false; }
    hr=g_device->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&g_ps); b->Release(); if(FAILED(hr)) return false;
    D3D11_SAMPLER_DESC ss{}; ss.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; ss.AddressU=ss.AddressV=ss.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    return SUCCEEDED(g_device->CreateSamplerState(&ss,&g_sampler));
}

void LogTexture(ID3D11ShaderResourceView* srv) {
    ID3D11Resource* res=nullptr; srv->GetResource(&res); ID3D11Texture2D* tex=nullptr;
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) { D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d); Log("Mirror texture: %ux%u format=%d bind=0x%X samples=%u usage=%d",d.Width,d.Height,(int)d.Format,d.BindFlags,d.SampleDesc.Count,(int)d.Usage); tex->Release(); }
    if(res)res->Release();
}

void Render() {
    if (RenderVirtualDisplay()) return;
    if (!g_compositor) return;
    void* raw=nullptr; vr::EVREye eye=g_rightEye?vr::Eye_Right:vr::Eye_Left;
    vr::EVRCompositorError ce=g_compositor->GetMirrorTextureD3D11(eye,g_device,&raw);
    if (ce!=vr::VRCompositorError_None || !raw) { static bool once=false; if(!once){Log("GetMirrorTextureD3D11(%s) failed: %d",g_rightEye?"right":"left",(int)ce);once=true;} return; }
    ID3D11ShaderResourceView* srv=static_cast<ID3D11ShaderResourceView*>(raw); static bool once=false; if(!once){LogTexture(srv);once=true;}
    RenderSource(srv); g_compositor->ReleaseMirrorTextureD3D11(srv);
}

LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_KEYDOWN && w=='2'){g_rightEye=!g_rightEye;Log("Eye: %s",g_rightEye?"right":"left");} if(m==WM_SIZE && g_swap){g_context->OMSetRenderTargets(0,nullptr,nullptr);if(g_rtv){g_rtv->Release();g_rtv=nullptr;}g_swap->ResizeBuffers(0,LOWORD(l),HIWORD(l),DXGI_FORMAT_UNKNOWN,0);ID3D11Texture2D*b=nullptr;g_swap->GetBuffer(0,IID_PPV_ARGS(&b));g_device->CreateRenderTargetView(b,nullptr,&g_rtv);b->Release();}if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProc(h,m,w,l);}
}

int main(){HINSTANCE hi=GetModuleHandleA(nullptr); int show=SW_SHOW; Log("BigscreenDesktopViewer starting");
    WNDCLASSA wc{};wc.hInstance=hi;wc.lpfnWndProc=WndProc;wc.lpszClassName="BigscreenDesktopViewer";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassA(&wc);
    g_hwnd=CreateWindowA(wc.lpszClassName,"Bigscreen Desktop Viewer",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,1280,720,nullptr,nullptr,hi,nullptr);ShowWindow(g_hwnd,show);
    if(!InitD3D()) {ReleaseAll();return 2;} g_virtualMapping=OpenFileMappingW(FILE_MAP_READ,FALSE,bigscreen_virtual_display_ipc::kMappingName); if(g_virtualMapping) g_virtualMapped=MapViewOfFile(g_virtualMapping,FILE_MAP_READ,0,0,bigscreen_virtual_display_ipc::kMappingSize); vr::EVRInitError err=vr::VRInitError_None;g_vr=vr::VR_Init(&err,vr::VRApplication_Background);if(!g_vr){Log("VR_Init failed: %s",vr::VR_GetVRInitErrorAsEnglishDescription(err));ReleaseAll();return 3;}g_compositor=static_cast<vr::IVRCompositor*>(vr::VR_GetGenericInterface(vr::IVRCompositor_Version,&err));if(!g_compositor){Log("Compositor interface unavailable: %s",vr::VR_GetVRInitErrorAsEnglishDescription(err));ReleaseAll();return 4;}Log("OpenVR compositor connected; virtual display mapping=%s",g_virtualMapped?"yes":"no");g_started=GetTickCount64();
    MSG msg{};while(msg.message!=WM_QUIT){while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}Render();}Log("Frames=%llu elapsed_ms=%llu",g_frames,GetTickCount64()-g_started);ReleaseAll();return 0;}
