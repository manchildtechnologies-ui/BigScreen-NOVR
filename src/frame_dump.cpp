#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

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

constexpr uint32_t kRawFrameMagic = 0x52464442;

bool SavePng(const BYTE* pixels, UINT width, UINT height, UINT stride, const std::wstring& path) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* bitmap = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool success = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA,
                                                   stride, stride * height, const_cast<BYTE*>(pixels), &bitmap))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) break;
        if (FAILED(frame->Initialize(nullptr))) break;
        if (FAILED(frame->SetSize(width, height))) break;
        GUID pixelFormat = GUID_WICPixelFormat32bppRGBA;
        if (FAILED(frame->SetPixelFormat(&pixelFormat))) break;
        if (FAILED(frame->WriteSource(bitmap, nullptr))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        success = true;
    } while (false);
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (bitmap) bitmap->Release();
    if (factory) factory->Release();
    if (uninitialize) CoUninitialize();
    return success;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: BigscreenFrameDump.exe frame.raw [output.png]\n");
        return 2;
    }
    const std::wstring inputPath = argv[1];
    std::ifstream input(inputPath, std::ios::binary | std::ios::ate);
    if (!input) {
        fwprintf(stderr, L"cannot open %ls\n", inputPath.c_str());
        return 3;
    }
    const std::streamsize fileSize = input.tellg();
    if (fileSize < static_cast<std::streamsize>(sizeof(RawFrameHeader))) {
        fwprintf(stderr, L"file is smaller than the raw header\n");
        return 4;
    }
    input.seekg(0);
    RawFrameHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != kRawFrameMagic || header.version != 1 || header.width == 0 || header.height == 0 ||
        header.rowPitch < header.width * 4 || header.byteCount != header.rowPitch * header.height ||
        fileSize != static_cast<std::streamsize>(sizeof(header) + header.byteCount)) {
        fwprintf(stderr, L"invalid raw frame header or byte count\n");
        return 5;
    }
    std::vector<BYTE> pixels(header.byteCount);
    input.read(reinterpret_cast<char*>(pixels.data()), pixels.size());
    if (!input) {
        fwprintf(stderr, L"raw pixel data is truncated\n");
        return 6;
    }

    uint64_t sum = 0;
    uint64_t nonBlackPixels = 0;
    BYTE minimum = 255;
    BYTE maximum = 0;
    for (uint32_t y = 0; y < header.height; ++y) {
        const BYTE* row = pixels.data() + static_cast<size_t>(y) * header.rowPitch;
        for (uint32_t x = 0; x < header.width; ++x) {
            const BYTE* pixel = row + x * 4;
            minimum = std::min({minimum, pixel[0], pixel[1], pixel[2]});
            maximum = std::max({maximum, pixel[0], pixel[1], pixel[2]});
            sum += pixel[0] + pixel[1] + pixel[2];
            if (pixel[0] || pixel[1] || pixel[2]) ++nonBlackPixels;
        }
    }
    const uint64_t pixelCount = static_cast<uint64_t>(header.width) * header.height;
    const double average = pixelCount ? static_cast<double>(sum) / (pixelCount * 3.0) : 0.0;
    const double nonBlackPercent = pixelCount ? 100.0 * static_cast<double>(nonBlackPixels) / pixelCount : 0.0;
    wprintf(L"valid=yes width=%u height=%u format=%u rowPitch=%u bytes=%u sequence=%llu\n",
            header.width, header.height, header.format, header.rowPitch, header.byteCount,
            static_cast<unsigned long long>(header.sequence));
    wprintf(L"min=%u max=%u average=%.3f nonBlackPixels=%.3f%% spatialRange=%u\n",
            minimum, maximum, average, nonBlackPercent, static_cast<unsigned>(maximum - minimum));

    if (argc >= 3) {
        const std::wstring outputPath = argv[2];
        if (!SavePng(pixels.data(), header.width, header.height, header.rowPitch, outputPath)) {
            fwprintf(stderr, L"PNG save failed\n");
            return 7;
        }
        wprintf(L"png=%ls\n", outputPath.c_str());
    }
    return 0;
}
