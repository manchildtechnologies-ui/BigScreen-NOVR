#include "pose_ipc.h"
#include "controller_ipc.h"

#include <Windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <conio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kPitchLimit = 80.0 * kPi / 180.0;
constexpr double kYawRate = 1.8;
constexpr double kPitchRate = 1.5;
constexpr double kDefaultMouseSensitivity = 0.0025;
constexpr float kControllerDeadzone = 0.15f;
constexpr double kXboxRightStickYawRate = 2.4;
constexpr double kXboxRightStickPitchRate = 2.0;

struct XboxState {
    bool connected = false;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    uint32_t buttons = 0;
    uint32_t dpad = 8;
};

float NormalizeAxis(ULONG value, const HIDP_VALUE_CAPS& caps) {
    ULONG inferredMax = 0;
    const LONG logicalMin = caps.LogicalMin;
    LONG logicalMax = caps.LogicalMax;
    if (logicalMax <= logicalMin && caps.BitSize > 0 && caps.BitSize < 32) {
        inferredMax = (1u << caps.BitSize) - 1u;
        logicalMax = static_cast<LONG>(inferredMax);
    }
    const float lo = static_cast<float>(logicalMin);
    const float hi = static_cast<float>(logicalMax);
    if (hi <= lo) return 0.0f;
    const float center = (lo + hi) * 0.5f;
    const float halfRange = (hi - lo) * 0.5f;
    return std::clamp((static_cast<float>(value) - center) / halfRange, -1.0f, 1.0f);
}

float NormalizeTrigger(ULONG value, LONG logicalMin, LONG logicalMax) {
    const float lo = static_cast<float>(logicalMin);
    const float hi = static_cast<float>(logicalMax);
    if (hi <= lo) return 0.0f;
    return std::clamp((static_cast<float>(value) - lo) / (hi - lo), 0.0f, 1.0f);
}

float ApplyControllerDeadzone(float value) {
    const float magnitude = std::fabs(value);
    if (magnitude <= kControllerDeadzone) return 0.0f;
    const float remapped = (magnitude - kControllerDeadzone) / (1.0f - kControllerDeadzone);
    return std::copysign(remapped, value);
}

class XboxHidReader final {
public:
    ~XboxHidReader() { Stop(); }

    void Start() {
        stop_.store(false);
        worker_ = std::thread(&XboxHidReader::Worker, this);
    }

    void Stop() {
        stop_.store(true);
        if (handle_ != INVALID_HANDLE_VALUE) CancelIoEx(handle_, nullptr);
        if (worker_.joinable()) worker_.join();
        CloseDevice();
    }

    XboxState Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    void ProcessRawInput(const RAWINPUT* raw) {
        if (!raw || raw->header.dwType != RIM_TYPEHID || raw->data.hid.dwSizeHid == 0) return;
        UINT pathLength = 0;
        GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, nullptr, &pathLength);
        if (!pathLength) return;
        std::vector<wchar_t> path(pathLength + 1);
        if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, path.data(), &pathLength) == static_cast<UINT>(-1)) return;
        std::wstring upper(path.data());
        std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return std::towupper(c); });
        if (upper.find(L"VID_045E") == std::wstring::npos || upper.find(L"PID_0B13") == std::wstring::npos) return;
        const BYTE* report = raw->data.hid.bRawData;
        const UINT reportCount = raw->data.hid.dwCount;
        for (UINT index = 0; index < reportCount; ++index) {
            ReadReport(report + index * raw->data.hid.dwSizeHid, raw->data.hid.dwSizeHid);
        }
    }

private:
    static constexpr USHORT kVendorId = 0x045E;
    static constexpr USHORT kProductId = 0x0B13;

    static std::string ErrorText(DWORD error) { return std::to_string(static_cast<unsigned long>(error)); }

    bool OpenDevice() {
        GUID hidGuid{};
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO info = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE) return false;

        bool opened = false;
        for (DWORD index = 0; !opened; ++index) {
            SP_DEVICE_INTERFACE_DATA interfaceData{};
            interfaceData.cbSize = sizeof(interfaceData);
            if (!SetupDiEnumDeviceInterfaces(info, nullptr, &hidGuid, index, &interfaceData)) break;
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, nullptr, 0, &required, nullptr);
            if (!required) continue;
            std::vector<BYTE> detailBuffer(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, required, nullptr, nullptr)) continue;

            std::wstring path(detail->DevicePath);
            std::wstring upper = path;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return std::towupper(c); });
            if (upper.find(L"VID_045E") == std::wstring::npos || upper.find(L"PID_0B13") == std::wstring::npos) continue;

            HANDLE candidate = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (candidate == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                if (diagnosedPaths_.insert(path).second) {
                    std::printf("Xbox HID interface path=%ls usagePage=unknown usage=unknown inputReportLength=unknown "
                                "CreateFile=FAIL error=%lu access=GENERIC_READ share=READ|WRITE flags=OVERLAPPED\n",
                                path.c_str(), static_cast<unsigned long>(error));
                }
                continue;
            }
            PHIDP_PREPARSED_DATA preparsed = nullptr;
            HIDP_CAPS caps{};
            if (!HidD_GetPreparsedData(candidate, &preparsed)) {
                const DWORD error = GetLastError();
                if (diagnosedPaths_.insert(path).second) {
                    std::printf("Xbox HID interface path=%ls usagePage=unknown usage=unknown inputReportLength=unknown "
                                "CreateFile=OK preparsed=FAIL error=%lu\n",
                                path.c_str(), static_cast<unsigned long>(error));
                }
                CloseHandle(candidate);
                continue;
            }
            const NTSTATUS capsStatus = HidP_GetCaps(preparsed, &caps);
            if (capsStatus != HIDP_STATUS_SUCCESS || !caps.InputReportByteLength) {
                if (diagnosedPaths_.insert(path).second) {
                    std::printf("Xbox HID interface path=%ls usagePage=0x%04X usage=0x%04X inputReportLength=%u "
                                "CreateFile=OK preparsed=OK caps=FAIL status=0x%08lX\n",
                                path.c_str(), caps.UsagePage, caps.Usage, caps.InputReportByteLength,
                                static_cast<unsigned long>(capsStatus));
                }
                if (preparsed) HidD_FreePreparsedData(preparsed);
                CloseHandle(candidate);
                continue;
            }

            const bool isGamepadCollection = caps.UsagePage == HID_USAGE_PAGE_GENERIC &&
                (caps.Usage == HID_USAGE_GENERIC_GAMEPAD || caps.Usage == HID_USAGE_GENERIC_JOYSTICK);
            if (diagnosedPaths_.insert(path).second) {
                std::printf("Xbox HID interface path=%ls usagePage=0x%04X usage=0x%04X inputReportLength=%u "
                            "CreateFile=OK preparsed=OK caps=OK gamepadCollection=%s access=GENERIC_READ "
                            "share=READ|WRITE flags=OVERLAPPED\n",
                            path.c_str(), caps.UsagePage, caps.Usage, caps.InputReportByteLength,
                            isGamepadCollection ? "yes" : "no");
            }
            if (!isGamepadCollection) {
                HidD_FreePreparsedData(preparsed);
                CloseHandle(candidate);
                continue;
            }

            std::vector<HIDP_VALUE_CAPS> valueCaps(caps.NumberInputValueCaps);
            USHORT valueCount = caps.NumberInputValueCaps;
            if (valueCount && HidP_GetValueCaps(HidP_Input, valueCaps.data(), &valueCount, preparsed) != HIDP_STATUS_SUCCESS) {
                HidD_FreePreparsedData(preparsed);
                CloseHandle(candidate);
                continue;
            }
            HIDP_VALUE_CAPS leftXCaps{}, leftYCaps{}, rightXCaps{}, rightYCaps{};
            HIDP_VALUE_CAPS leftTriggerCaps{}, rightTriggerCaps{}, dpadCaps{};
            std::printf("Xbox HID gamepad value usages:");
            for (const auto& value : valueCaps) {
                if (value.IsRange || value.UsagePage != HID_USAGE_PAGE_GENERIC) continue;
                std::printf(" page=0x%02X usage=0x%02X link=%u range=%ld..%ld",
                            value.UsagePage, value.NotRange.Usage, value.LinkCollection,
                            value.LogicalMin, value.LogicalMax);
                switch (value.NotRange.Usage) {
                case HID_USAGE_GENERIC_X: leftXCaps = value; break;
                case HID_USAGE_GENERIC_Y: leftYCaps = value; break;
                case HID_USAGE_GENERIC_RX: rightXCaps = value; break;
                case HID_USAGE_GENERIC_RY: rightYCaps = value; break;
                case HID_USAGE_GENERIC_Z: leftTriggerCaps = value; break;
                case HID_USAGE_GENERIC_RZ: rightTriggerCaps = value; break;
                case HID_USAGE_GENERIC_HATSWITCH: dpadCaps = value; break;
                default: break;
                }
            }
            std::printf("\n");
            if (!rightXCaps.NotRange.Usage || !rightYCaps.NotRange.Usage) {
                std::printf("Xbox HID found but RX/RY usages were not present\n");
                HidD_FreePreparsedData(preparsed);
                CloseHandle(candidate);
                continue;
            }
            leftXCaps_ = leftXCaps;
            leftYCaps_ = leftYCaps;
            rightXCaps_ = rightXCaps;
            rightYCaps_ = rightYCaps;
            leftTriggerCaps_ = leftTriggerCaps;
            rightTriggerCaps_ = rightTriggerCaps;
            dpadCaps_ = dpadCaps;
            handle_ = candidate;
            preparsed_ = preparsed;
            caps_ = caps;
            path_ = path;
            opened = true;
            std::printf("Xbox HID connected: VID_045E PID_0B13 inputReport=%u\n", caps_.InputReportByteLength);
        }
        SetupDiDestroyDeviceInfoList(info);
        return opened;
    }

    bool GetValue(const HIDP_VALUE_CAPS& caps, const BYTE* report, DWORD bytes, ULONG& value) const {
        if (!caps.NotRange.Usage) return false;
        return HidP_GetUsageValue(HidP_Input, caps.UsagePage, caps.LinkCollection,
                                  caps.NotRange.Usage, &value, preparsed_,
                                  reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), bytes) == HIDP_STATUS_SUCCESS;
    }

    void ReadReport(const BYTE* report, DWORD bytes) {
        XboxState next = Snapshot();
        const XboxState previous = next;
        next.connected = true;
        ULONG value = 0;
        if (GetValue(leftXCaps_, report, bytes, value)) next.leftX = NormalizeAxis(value, leftXCaps_);
        if (GetValue(leftYCaps_, report, bytes, value)) next.leftY = NormalizeAxis(value, leftYCaps_);
        if (GetValue(rightXCaps_, report, bytes, value)) next.rightX = NormalizeAxis(value, rightXCaps_);
        if (GetValue(rightYCaps_, report, bytes, value)) next.rightY = NormalizeAxis(value, rightYCaps_);
        if (GetValue(leftTriggerCaps_, report, bytes, value)) next.leftTrigger = NormalizeTrigger(value, leftTriggerCaps_.LogicalMin, leftTriggerCaps_.LogicalMax);
        if (GetValue(rightTriggerCaps_, report, bytes, value)) next.rightTrigger = NormalizeTrigger(value, rightTriggerCaps_.LogicalMin, rightTriggerCaps_.LogicalMax);
        if (GetValue(dpadCaps_, report, bytes, value)) next.dpad = value;

        USAGE usages[32]{};
        ULONG usageCount = 32;
        if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usages, &usageCount,
                           preparsed_, reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), bytes) == HIDP_STATUS_SUCCESS) {
            next.buttons = 0;
            for (ULONG i = 0; i < usageCount; ++i) {
                switch (usages[i]) {
                case 1: next.buttons |= bigscreen_desktop_controller_ipc::Button_A; break;
                case 2: next.buttons |= bigscreen_desktop_controller_ipc::Button_B; break;
                case 3: next.buttons |= bigscreen_desktop_controller_ipc::Button_X; break;
                case 4: next.buttons |= bigscreen_desktop_controller_ipc::Button_Y; break;
                case 5: next.buttons |= bigscreen_desktop_controller_ipc::Button_LeftBumper; break;
                case 6: next.buttons |= bigscreen_desktop_controller_ipc::Button_RightBumper; break;
                case 7: next.buttons |= bigscreen_desktop_controller_ipc::Button_View; break;
                case 8: next.buttons |= bigscreen_desktop_controller_ipc::Button_Menu; break;
                case 9: next.buttons |= bigscreen_desktop_controller_ipc::Button_LeftStick; break;
                case 10: next.buttons |= bigscreen_desktop_controller_ipc::Button_RightStick; break;
                default: break;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = next;
        }
        ++reportCount_;
        const bool rawChanged = !rawReportReported_ || lastRawReport_.size() != bytes ||
            std::memcmp(lastRawReport_.data(), report, bytes) != 0;
        if (rawChanged) {
            std::printf("[HID-RAW] bytes=%lu data=", static_cast<unsigned long>(bytes));
            for (DWORD i = 0; i < bytes; ++i) std::printf("%02X", report[i]);
            std::printf("\n");
            lastRawReport_.assign(report, report + bytes);
            rawReportReported_ = true;
        }
        const bool hidA = (next.buttons & bigscreen_desktop_controller_ipc::Button_A) != 0;
        if (!aReported_ || hidA != lastA_) {
            std::printf("[A-TRACE] HID A=%s\n", hidA ? "DOWN" : "UP");
            lastA_ = hidA;
            aReported_ = true;
        }
        if (reportCount_ == 1 || std::fabs(next.rightX - previous.rightX) > 0.02f ||
            std::fabs(next.rightY - previous.rightY) > 0.02f || next.buttons != previous.buttons) {
            std::printf("Xbox raw report bytes=%lu RX=%+.3f RY=%+.3f LX=%+.3f LY=%+.3f "
                        "LT=%.3f RT=%.3f buttons=0x%03X dpad=%u\n",
                        static_cast<unsigned long>(bytes), next.rightX, next.rightY,
                        next.leftX, next.leftY, next.leftTrigger, next.rightTrigger,
                        next.buttons, next.dpad);
        }
    }

    void CloseDevice() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.connected = false;
        }
        if (preparsed_) HidD_FreePreparsedData(preparsed_);
        preparsed_ = nullptr;
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

    void Worker() {
        while (!stop_.load()) {
            if (handle_ == INVALID_HANDLE_VALUE && !OpenDevice()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            std::vector<BYTE> report(caps_.InputReportByteLength);
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(handle_, report.data(), static_cast<DWORD>(report.size()), &bytesRead, &overlapped);
            if (!ok && GetLastError() != ERROR_IO_PENDING) {
                CloseHandle(overlapped.hEvent);
                CloseDevice();
                continue;
            }
            DWORD wait = WAIT_TIMEOUT;
            while (wait == WAIT_TIMEOUT && !stop_.load()) wait = WaitForSingleObject(overlapped.hEvent, 100);
            if (wait == WAIT_OBJECT_0 && GetOverlappedResult(handle_, &overlapped, &bytesRead, FALSE) && bytesRead) {
                ReadReport(report.data(), bytesRead);
            } else if (wait == WAIT_OBJECT_0 && !stop_.load()) {
                const DWORD error = GetLastError();
                std::printf("Xbox HID GetOverlappedResult/read completion failed error=%lu\n",
                            static_cast<unsigned long>(error));
                CancelIoEx(handle_, &overlapped);
                CloseHandle(overlapped.hEvent);
                CloseDevice();
                continue;
            }
            if (wait == WAIT_FAILED && !stop_.load()) {
                std::printf("Xbox HID WaitForSingleObject failed error=%lu\n",
                            static_cast<unsigned long>(GetLastError()));
                CancelIoEx(handle_, &overlapped);
                CloseHandle(overlapped.hEvent);
                CloseDevice();
                continue;
            }
            if (stop_.load()) CancelIoEx(handle_, &overlapped);
            CloseHandle(overlapped.hEvent);
        }
        CloseDevice();
    }

    mutable std::mutex mutex_;
    XboxState state_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    PHIDP_PREPARSED_DATA preparsed_ = nullptr;
    HIDP_CAPS caps_{};
    HIDP_VALUE_CAPS leftXCaps_{}, leftYCaps_{}, rightXCaps_{}, rightYCaps_{};
    HIDP_VALUE_CAPS leftTriggerCaps_{}, rightTriggerCaps_{}, dpadCaps_{};
    std::wstring path_;
    std::unordered_set<std::wstring> diagnosedPaths_;
    uint64_t reportCount_ = 0;
    bool rawReportReported_ = false;
    std::vector<BYTE> lastRawReport_;
    bool aReported_ = false;
    bool lastA_ = false;
};

class WindowsGamepadReader final {
public:
    bool Available() const { return available_; }

    XboxState Snapshot() {
        XboxState result;
        try {
            const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
            const uint32_t count = gamepads.Size();
            available_ = count != 0;
            if (count != lastCount_) {
                std::printf("[WGI-TRACE] gamepad count=%u\n", count);
                lastCount_ = count;
                if (count == 0) pad_ = nullptr;
            }
            if (count == 0) return result;
            if (!pad_) {
                pad_ = gamepads.GetAt(0);
                std::printf("[WGI-TRACE] selected Gamepad::Gamepads()[0]\n");
            }
            const auto reading = pad_.GetCurrentReading();
            result.connected = true;
            result.leftX = static_cast<float>(reading.LeftThumbstickX);
            result.leftY = static_cast<float>(reading.LeftThumbstickY);
            result.rightX = static_cast<float>(reading.RightThumbstickX);
            result.rightY = static_cast<float>(reading.RightThumbstickY);
            result.leftTrigger = static_cast<float>(reading.LeftTrigger);
            result.rightTrigger = static_cast<float>(reading.RightTrigger);
            const uint32_t buttons = static_cast<uint32_t>(reading.Buttons);
            const auto has = [buttons](winrt::Windows::Gaming::Input::GamepadButtons button) {
                return (buttons & static_cast<uint32_t>(button)) != 0;
            };
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::A)) result.buttons |= bigscreen_desktop_controller_ipc::Button_A;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::B)) result.buttons |= bigscreen_desktop_controller_ipc::Button_B;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::X)) result.buttons |= bigscreen_desktop_controller_ipc::Button_X;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::Y)) result.buttons |= bigscreen_desktop_controller_ipc::Button_Y;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::LeftShoulder)) result.buttons |= bigscreen_desktop_controller_ipc::Button_LeftBumper;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::RightShoulder)) result.buttons |= bigscreen_desktop_controller_ipc::Button_RightBumper;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::LeftThumbstick)) result.buttons |= bigscreen_desktop_controller_ipc::Button_LeftStick;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::RightThumbstick)) result.buttons |= bigscreen_desktop_controller_ipc::Button_RightStick;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::View)) result.buttons |= bigscreen_desktop_controller_ipc::Button_View;
            if (has(winrt::Windows::Gaming::Input::GamepadButtons::Menu)) result.buttons |= bigscreen_desktop_controller_ipc::Button_Menu;
            result.dpad = buttons & (0x40u | 0x80u | 0x100u | 0x200u);
            if (!rawReported_ || std::fabs(result.rightX - rawRightX_) > 0.01f ||
                std::fabs(result.rightY - rawRightY_) > 0.01f || result.buttons != rawButtons_) {
                std::printf("[WGI-TRACE] reading=OK buttons=0x%03X RX=%+.3f RY=%+.3f\n",
                            result.buttons, result.rightX, result.rightY);
                rawRightX_ = result.rightX;
                rawRightY_ = result.rightY;
                rawButtons_ = result.buttons;
                rawReported_ = true;
            }
            const bool a = (result.buttons & bigscreen_desktop_controller_ipc::Button_A) != 0;
            if (!aReported_ || a != lastA_) {
                std::printf("[A-TRACE] Windows.Gaming.Input A=%s\n", a ? "DOWN" : "UP");
                lastA_ = a;
                aReported_ = true;
            }
            if (!readingReported_ || std::fabs(result.leftX - last_.leftX) > 0.01f ||
                std::fabs(result.leftY - last_.leftY) > 0.01f ||
                std::fabs(result.rightX - last_.rightX) > 0.01f ||
                std::fabs(result.rightY - last_.rightY) > 0.01f ||
                std::fabs(result.leftTrigger - last_.leftTrigger) > 0.01f ||
                std::fabs(result.rightTrigger - last_.rightTrigger) > 0.01f ||
                result.buttons != last_.buttons || result.dpad != last_.dpad) {
                std::printf("Windows.Gaming.Input reading timestamp=%llu LX=%+.3f LY=%+.3f "
                            "RX=%+.3f RY=%+.3f LT=%.3f RT=%.3f buttons=0x%03X dpad=0x%03X\n",
                            static_cast<unsigned long long>(reading.Timestamp), result.leftX, result.leftY,
                            result.rightX, result.rightY, result.leftTrigger, result.rightTrigger,
                            result.buttons, result.dpad);
                last_ = result;
                readingReported_ = true;
            }
            if (!reported_) {
                std::printf("Windows.Gaming.Input: Gamepad detected count=%u\n", gamepads.Size());
                reported_ = true;
            }
        } catch (const winrt::hresult_error& error) {
            const unsigned code = static_cast<unsigned>(error.code().value);
            if (!readErrorReported_ || code != lastReadError_) {
                std::printf("[WGI-TRACE] reading=FAILED error=0x%08X\n", code);
                lastReadError_ = code;
                readErrorReported_ = true;
            }
        }
        return result;
    }

private:
    bool reported_ = false;
    bool readingReported_ = false;
    XboxState last_;
    bool aReported_ = false;
    bool lastA_ = false;
    winrt::Windows::Gaming::Input::Gamepad pad_{nullptr};
    uint32_t lastCount_ = 0;
    bool available_ = false;
    bool rawReported_ = false;
    float rawRightX_ = 0.0f;
    float rawRightY_ = 0.0f;
    uint32_t rawButtons_ = 0;
    bool readErrorReported_ = false;
    unsigned lastReadError_ = 0;
};

double ReadMouseSensitivity() {
    const char* value = std::getenv("BIGSCREEN_MOUSE_SENSITIVITY");
    if (!value || !*value) return kDefaultMouseSensitivity;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    return end != value && *end == '\0' && std::isfinite(parsed) && parsed > 0.0
        ? parsed : kDefaultMouseSensitivity;
}

bool RegisterRawMouseInput(HWND target) {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = target;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) == TRUE;
}

bool RegisterRawControllerInput(HWND target) {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x05;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = target;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) == TRUE;
}

HWND CreateRawInputWindow() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.lpszClassName = L"BigscreenDesktopRawInputWindow";
    RegisterClassW(&windowClass);
    return CreateWindowExW(0, windowClass.lpszClassName, L"Bigscreen Desktop Raw Input",
                           0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
}

void ProcessRawMouse(HWND target, bool active, double sensitivity, double& yaw, double& pitch, XboxHidReader* xboxReader) {
    MSG message{};
    (void)target;
    while (PeekMessage(&message, nullptr, WM_INPUT, WM_INPUT, PM_REMOVE)) {
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
                            nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) continue;
        std::vector<BYTE> buffer(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
                            buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) continue;
        const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (raw->header.dwType == RIM_TYPEMOUSE) {
            if (!active) continue;
            yaw += static_cast<double>(raw->data.mouse.lLastX) * sensitivity;
            pitch -= static_cast<double>(raw->data.mouse.lLastY) * sensitivity;
            pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);
        } else if (xboxReader) {
            xboxReader->ProcessRawInput(raw);
        }
    }
}

void PrintStatus(double yaw, double pitch, bool connected, bool mouseLookActive, double sensitivity, const XboxState& xbox) {
    std::printf("\rYaw: %7.2f deg   Pitch: %7.2f deg   IPC: %s   Xbox: %s LX=%+.2f LY=%+.2f RX=%+.2f RY=%+.2f LT=%.2f RT=%.2f BTN=0x%03X DP=%u   Mouse: %s   [Arrows | F8 mouse | R reset | ESC release/exit]   ",
                yaw * 180.0 / kPi, pitch * 180.0 / kPi, connected ? "active" : "starting",
                xbox.connected ? "connected" : "disconnected", xbox.leftX, xbox.leftY, xbox.rightX, xbox.rightY,
                xbox.leftTrigger, xbox.rightTrigger, xbox.buttons, xbox.dpad,
                mouseLookActive ? "active" : "off");
    std::fflush(stdout);
}
}

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace bigscreen_desktop_ipc;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, static_cast<DWORD>(sizeof(PoseState)), kMappingName);
    if (!mapping) {
        std::fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
        return 1;
    }
    auto* state = static_cast<PoseState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PoseState)));
    if (!state) {
        std::fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    *state = PoseState{};
    Publish(state, 0.0, 0.0, true);

    HANDLE controllerMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                                   0, static_cast<DWORD>(sizeof(bigscreen_desktop_controller_ipc::ControllerState)),
                                                   bigscreen_desktop_controller_ipc::kMappingName);
    if (!controllerMapping) {
        std::fprintf(stderr, "CreateFileMapping controller failed: %lu\n", GetLastError());
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        return 1;
    }
    auto* controllerState = static_cast<bigscreen_desktop_controller_ipc::ControllerState*>(MapViewOfFile(
        controllerMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bigscreen_desktop_controller_ipc::ControllerState)));
    if (!controllerState) {
        std::fprintf(stderr, "MapViewOfFile controller failed: %lu\n", GetLastError());
        CloseHandle(controllerMapping);
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        return 1;
    }
    *controllerState = bigscreen_desktop_controller_ipc::ControllerState{};
    bigscreen_desktop_controller_ipc::Publish(controllerState, false, 0, 0, 0, 0, 0, 0, 0, 8);
    std::printf("[A-TRACE] IPC layout size=%zu magic=0x%08X version=%u\n",
                sizeof(bigscreen_desktop_controller_ipc::ControllerState),
                bigscreen_desktop_controller_ipc::kMagic,
                bigscreen_desktop_controller_ipc::kVersion);

    XboxHidReader xboxReader;
    xboxReader.Start();
    WindowsGamepadReader windowsGamepadReader;
    std::printf("BigscreenDesktopBridge keyboard input\n");
    std::printf("Shared memory: %ls\n", kMappingName);
    std::printf("Controller IPC: %ls\n", bigscreen_desktop_controller_ipc::kMappingName);

    const HWND consoleWindow = GetConsoleWindow();
    const HWND rawInputWindow = CreateRawInputWindow();
    const double mouseSensitivity = ReadMouseSensitivity();
    const HWND rawTarget = rawInputWindow ? rawInputWindow : consoleWindow;
    const bool rawMouseReady = rawTarget && RegisterRawMouseInput(rawTarget);
    const bool rawControllerReady = rawTarget && RegisterRawControllerInput(rawTarget);
    std::printf("Mouse look: F8 toggles, ESC releases, sensitivity=%.4f rad/count, raw input=%s\n",
                mouseSensitivity, rawMouseReady ? "ready" : "unavailable");
    std::printf("Xbox Raw Input: %s\n", rawControllerReady ? "ready" : "unavailable");

    bool running = true;
    bool mouseLookActive = false;
    double yaw = 0.0;
    double pitch = 0.0;
    uint32_t previousButtons = 0;
    bool publishedA = false;
    ULONGLONG lastTick = GetTickCount64();
    ULONGLONG lastDisplay = 0;
    while (running) {
        const ULONGLONG now = GetTickCount64();
        const double dt = std::clamp(static_cast<double>(now - lastTick) / 1000.0, 0.0, 0.1);
        lastTick = now;

        if (GetAsyncKeyState(VK_F8) & 0x0001) mouseLookActive = !mouseLookActive;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x0001) {
            if (mouseLookActive) mouseLookActive = false;
            else running = false;
        }
        if (GetAsyncKeyState('R') & 0x0001) {
            yaw = 0.0;
            pitch = 0.0;
        }
        if (rawMouseReady || rawControllerReady) {
            ProcessRawMouse(consoleWindow, mouseLookActive, mouseSensitivity, yaw, pitch, &xboxReader);
        }
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) yaw -= kYawRate * dt;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) yaw += kYawRate * dt;
        if (GetAsyncKeyState(VK_UP) & 0x8000) pitch += kPitchRate * dt;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) pitch -= kPitchRate * dt;
        const XboxState hidXbox = xboxReader.Snapshot();
        const XboxState gamepadXbox = windowsGamepadReader.Snapshot();
        XboxState xbox = windowsGamepadReader.Available() ? gamepadXbox : hidXbox;
        if (hidXbox.connected) {
            xbox.connected = true;
            xbox.rightX = hidXbox.rightX;
            xbox.rightY = hidXbox.rightY;
            xbox.buttons = (xbox.buttons & ~bigscreen_desktop_controller_ipc::Button_A) |
                           (hidXbox.buttons & bigscreen_desktop_controller_ipc::Button_A);
        }
        const float rightX = ApplyControllerDeadzone(xbox.rightX);
        const float rightY = ApplyControllerDeadzone(xbox.rightY);
        if (xbox.connected) {
            yaw += static_cast<double>(rightX) * kXboxRightStickYawRate * dt;
            pitch -= static_cast<double>(rightY) * kXboxRightStickPitchRate * dt;
            if ((xbox.buttons & bigscreen_desktop_controller_ipc::Button_RightStick) &&
                !(previousButtons & bigscreen_desktop_controller_ipc::Button_RightStick)) {
                yaw = 0.0;
                pitch = 0.0;
            }
        }
        previousButtons = xbox.buttons;
        pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);
        Publish(state, yaw, pitch, running);
        bigscreen_desktop_controller_ipc::Publish(controllerState, xbox.connected,
                                                  xbox.leftX, xbox.leftY, xbox.rightX, xbox.rightY,
                                                  xbox.leftTrigger, xbox.rightTrigger, xbox.buttons, xbox.dpad);
        const bool currentA = (xbox.buttons & bigscreen_desktop_controller_ipc::Button_A) != 0;
        if (currentA != publishedA) {
            std::printf("[A-TRACE] IPC publish A=%s connected=%s sequence=%ld\n",
                        currentA ? "DOWN" : "UP", xbox.connected ? "yes" : "no",
                        controllerState->sequence);
            publishedA = currentA;
        }

        if (now - lastDisplay >= 250) {
            PrintStatus(yaw, pitch, true, mouseLookActive, mouseSensitivity, xbox);
            lastDisplay = now;
        }
        Sleep(10);
    }

    Publish(state, yaw, pitch, false);
    bigscreen_desktop_controller_ipc::Publish(controllerState, false, 0, 0, 0, 0, 0, 0, 0, 8);
    xboxReader.Stop();
    std::printf("\nBridge exited.\n");
    UnmapViewOfFile(controllerState);
    CloseHandle(controllerMapping);
    if (rawInputWindow) DestroyWindow(rawInputWindow);
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    winrt::uninit_apartment();
    return 0;
}
