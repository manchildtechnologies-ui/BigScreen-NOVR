#include <openvr_driver.h>
#include <Windows.h>
#include "controller_ipc.h"
#include "pose_ipc.h"
#include "direct_mode.h"
#include "bindings.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kStickScale = 1.0f / 32767.0f;

struct Config {
    float deadzone = 0.15f;
    float sensitivity = 2.2f;
    float pitchLimit = 80.0f * static_cast<float>(kPi / 180.0);
    double logInterval = 1.0;
};

Config g_config;

uint32_t CanonicalDpadMask(uint32_t dpad) {
    if (dpad > 7u) return dpad & (0x40u | 0x80u | 0x100u | 0x200u);
    switch (dpad) {
    case 0: return 0x40u;
    case 2: return 0x200u;
    case 4: return 0x80u;
    case 6: return 0x100u;
    default: return 0;
    }
}

bool ConfiguredGamepadAction(const std::map<bigscreen_bindings::Action, bigscreen_bindings::Binding>& bindings,
                             bigscreen_bindings::Action action,
                             const bigscreen_desktop_controller_ipc::ControllerState& controller) {
    const auto it = bindings.find(action);
    if (it == bindings.end() || it->second.device != bigscreen_bindings::Device::Gamepad) return false;
    const int code = it->second.code;
    if (code == 4) return controller.leftTrigger > 0.5f;
    if (code == 5) return controller.rightTrigger > 0.5f;
    if (code >= 0x40) return (CanonicalDpadMask(controller.dpad) & static_cast<uint32_t>(code)) != 0;
    return code >= 0 && code < 32 && (controller.buttons & (1u << code)) != 0;
}
vr::IVRServerDriverHost* g_host = nullptr;
vr::IVRDriverInput* g_input = nullptr;
vr::CVRPropertyHelpers* g_properties = nullptr;

// Third-person-style camera offset, expressed in the yaw-relative world frame.
// OpenVR view-forward is local -Z, so positive local Z places the camera behind
// the player while preserving the existing yaw/pitch orientation.
constexpr double kCameraBackOffsetMeters = 2.5;
constexpr double kCameraHeightOffsetMeters = 0.9;
constexpr double kArmAdjustRateMetersPerSecond = 0.8;

void ApplyLegacyHmdOffset(double yaw, double& x, double& y, double& z) {
    x += std::sin(yaw) * kCameraBackOffsetMeters;
    y += kCameraHeightOffsetMeters;
    z += std::cos(yaw) * kCameraBackOffsetMeters;
}

#if 0 // Experimental direct HID polling retained only for historical reference; Phase 7F uses IPC.
class HidXboxInput final {
public:
    ~HidXboxInput() { Stop(); }

    void Start() {
        stop_.store(false);
        worker_ = std::thread(&HidXboxInput::Worker, this);
    }

    void Stop() {
        stop_.store(true);
        if (handle_ != INVALID_HANDLE_VALUE) CancelIoEx(handle_, nullptr);
        if (worker_.joinable()) worker_.join();
        CloseDevice();
    }

    bool Read(float& x, float& y) const {
        if (!connected_.load()) return false;
        x = rx_.load();
        y = ry_.load();
        return true;
    }

private:
    static constexpr USHORT kVendorId = 0x045E;
    static constexpr USHORT kProductId = 0x0B13;

    static float Normalize(ULONG value, LONG logicalMin, LONG logicalMax) {
        const float lo = static_cast<float>(logicalMin);
        const float hi = static_cast<float>(logicalMax);
        if (hi <= lo) return 0.0f;
        const float center = (lo + hi) * 0.5f;
        const float halfRange = (hi - lo) * 0.5f;
        return std::clamp((static_cast<float>(value) - center) / halfRange, -1.0f, 1.0f);
    }

    bool OpenDevice() {
        GUID hidGuid{};
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO info = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE) return false;

        bool opened = false;
        for (DWORD index = 0; !opened; ++index) {
            SP_DEVICE_INTERFACE_DATA interfaceData{};
            interfaceData.cbSize = sizeof(interfaceData);
            if (!SetupDiEnumDeviceInterfaces(info, nullptr, &hidGuid, index, &interfaceData)) {
                break;
            }

            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, nullptr, 0, &required, nullptr);
            if (!required) continue;
            std::vector<BYTE> detailBuffer(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, required, nullptr, nullptr)) {
                continue;
            }

            std::wstring devicePath(detail->DevicePath);
            std::wstring upperPath = devicePath;
            std::transform(upperPath.begin(), upperPath.end(), upperPath.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
            const bool looksLikeXbox = upperPath.find(L"VID_045E") != std::wstring::npos &&
                                        upperPath.find(L"PID_0B13") != std::wstring::npos;
            if (looksLikeXbox) vr::VRDriverLog()->Log("Found Xbox HID interface");

            HANDLE candidate = CreateFileW(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                           0, nullptr);
            if (candidate == INVALID_HANDLE_VALUE) {
                candidate = CreateFileW(devicePath.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        0, nullptr);
            }
            if (candidate == INVALID_HANDLE_VALUE) {
                if (looksLikeXbox) vr::VRDriverLog()->Log(("Xbox HID open failed error=" + std::to_string(GetLastError())).c_str());
                continue;
            }

            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof(attributes);
            const bool attributesMatch = HidD_GetAttributes(candidate, &attributes) &&
                                         attributes.VendorID == kVendorId && attributes.ProductID == kProductId;
            if (!attributesMatch && !looksLikeXbox) {
                if (looksLikeXbox) vr::VRDriverLog()->Log("Xbox HID attributes did not match VID/PID");
                CloseHandle(candidate);
                continue;
            }
            if (!attributesMatch && looksLikeXbox) {
                vr::VRDriverLog()->Log("Xbox HID identified by VID/PID device path");
            }

            PHIDP_PREPARSED_DATA preparsed = nullptr;
            if (!HidD_GetPreparsedData(candidate, &preparsed)) {
                CloseHandle(candidate);
                continue;
            }
            HIDP_CAPS caps{};
            if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS || !caps.InputReportByteLength) {
                HidD_FreePreparsedData(preparsed);
                CloseHandle(candidate);
                continue;
            }

            USHORT valueCount = caps.NumberInputValueCaps;
            std::vector<HIDP_VALUE_CAPS> valueCaps(valueCount);
            if (valueCount && HidP_GetValueCaps(HidP_Input, valueCaps.data(), &valueCount, preparsed) != HIDP_STATUS_SUCCESS) {
                HidD_FreePreparsedData(preparsed);
                CloseHandle(candidate);
                continue;
            }

            bool foundX = false;
            bool foundY = false;
            for (const auto& value : valueCaps) {
                if (value.IsRange || value.UsagePage != HID_USAGE_PAGE_GENERIC) continue;
                vr::VRDriverLog()->Log(("HID cap usage=" + std::to_string(value.NotRange.Usage) +
                                        " logical=" + std::to_string(value.LogicalMin) + ".." +
                                        std::to_string(value.LogicalMax)).c_str());
                if (value.NotRange.Usage == HID_USAGE_GENERIC_RX) {
                    rxCaps_ = value;
                    foundX = true;
                } else if (value.NotRange.Usage == HID_USAGE_GENERIC_RY) {
                    ryCaps_ = value;
                    foundY = true;
                }
            }
            if (!foundX || !foundY) {
                vr::VRDriverLog()->Log("Xbox HID right-stick RX/RY usages not found");
                HidD_FreePreparsedData(preparsed);
                CloseHandle(candidate);
                continue;
            }

            CloseHandle(candidate);
            handle_ = CreateFileW(devicePath.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  0, nullptr);
            if (handle_ == INVALID_HANDLE_VALUE) {
                vr::VRDriverLog()->Log(("Xbox HID overlapped open failed error=" + std::to_string(GetLastError())).c_str());
                HidD_FreePreparsedData(preparsed);
                continue;
            }
            preparsed_ = preparsed;
            caps_ = caps;
            connected_.store(true);
            vr::VRDriverLog()->Log("Wireless Xbox HID detected VID=045E PID=0B13");
            vr::VRDriverLog()->Log("Input backend: HID");
            opened = true;
        }
        SetupDiDestroyDeviceInfoList(info);
        return opened;
    }

    void CloseDevice() {
        connected_.store(false);
        if (preparsed_) {
            HidD_FreePreparsedData(preparsed_);
            preparsed_ = nullptr;
        }
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    void Worker() {
        while (!stop_.load()) {
            if (handle_ == INVALID_HANDLE_VALUE && !OpenDevice()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            std::vector<BYTE> report(caps_.InputReportByteLength);
            DWORD bytesRead = 0;
            BOOL readSucceeded = ReadFile(handle_, report.data(), static_cast<DWORD>(report.size()),
                                          &bytesRead, nullptr);
            if (!readSucceeded || !bytesRead) {
                const DWORD error = GetLastError();
                if (stop_.load()) break;
                CloseDevice();
                vr::VRDriverLog()->Log(("Xbox HID read failed error=" + std::to_string(error)).c_str());
                continue;
            }
            ULONG value = 0;
            if (HidP_GetUsageValue(HidP_Input, rxCaps_.UsagePage, rxCaps_.LinkCollection,
                                   rxCaps_.NotRange.Usage, &value, preparsed_, reinterpret_cast<PCHAR>(report.data()),
                                   static_cast<ULONG>(bytesRead)) == HIDP_STATUS_SUCCESS) {
                rx_.store(Normalize(value, rxCaps_.LogicalMin, rxCaps_.LogicalMax));
            }
            if (HidP_GetUsageValue(HidP_Input, ryCaps_.UsagePage, ryCaps_.LinkCollection,
                                   ryCaps_.NotRange.Usage, &value, preparsed_, reinterpret_cast<PCHAR>(report.data()),
                                   static_cast<ULONG>(bytesRead)) == HIDP_STATUS_SUCCESS) {
                ry_.store(Normalize(value, ryCaps_.LogicalMin, ryCaps_.LogicalMax));
            }
        }
        CloseDevice();
    }

    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<float> rx_{0.0f};
    std::atomic<float> ry_{0.0f};
    std::thread worker_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    PHIDP_PREPARSED_DATA preparsed_ = nullptr;
    HIDP_CAPS caps_{};
    HIDP_VALUE_CAPS rxCaps_{};
    HIDP_VALUE_CAPS ryCaps_{};
};

HidXboxInput g_hidInput;
#endif

float ApplyDeadzone(SHORT value, float deadzone) {
    const float normalized = std::clamp(static_cast<float>(value) * kStickScale, -1.0f, 1.0f);
    const float magnitude = std::fabs(normalized);
    if (magnitude <= deadzone) return 0.0f;
    const float remapped = (magnitude - deadzone) / (1.0f - deadzone);
    return std::copysign(remapped, normalized);
}

vr::HmdQuaternion_t QuaternionFromYawPitch(double yaw, double pitch) {
    // Compose world-up yaw first, then local-X pitch.  This keeps roll at zero
    // instead of accumulating arbitrary quaternion rotations.
    const double cy = std::cos(yaw * 0.5), sy = std::sin(yaw * 0.5);
    const double cp = std::cos(pitch * 0.5), sp = std::sin(pitch * 0.5);
    vr::HmdQuaternion_t q{};
    q.w = cy * cp;
    q.x = cy * sp;
    q.y = sy * cp;
    q.z = -sy * sp;
    return q;
}

class DisplayComponent final : public vr::IVRDisplayComponent {
public:
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) override {
        *x = 0; *y = 0; *width = 2160; *height = 1200;
    }
    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override {
        *width = 2160; *height = 1200;
    }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) override {
        *x = eye == vr::Eye_Left ? 0 : 1080; *y = 0; *width = 1080; *height = 1200;
    }
    void GetProjectionRaw(vr::EVREye, float* left, float* right, float* top, float* bottom) override {
        // Wider projection for the stronger third-person-style desktop view.
        *left = -1.3f; *right = 1.3f; *top = -1.3f; *bottom = 1.3f;
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        vr::DistortionCoordinates_t result{};
        result.rfRed[0] = result.rfGreen[0] = result.rfBlue[0] = u;
        result.rfRed[1] = result.rfGreen[1] = result.rfBlue[1] = v;
        return result;
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t* result, vr::EVREye, uint32_t, float u, float v) override {
        result->v[0] = u;
        result->v[1] = v;
        return true;
    }
};

class VirtualHmd final : public vr::ITrackedDeviceServerDriver {
public:
    ~VirtualHmd() { ClosePoseIpc(); }

    vr::EVRInitError Activate(uint32_t objectId) override {
        index_ = objectId;
        props_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_TrackingSystemName_String, "BigscreenDesktopBridge");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ModelNumber_String, "Bigscreen Desktop HMD");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ManufacturerName_String, "BigscreenDesktopBridge");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ResourceRoot_String, "");
        vr::VRProperties()->SetBoolProperty(props_, vr::Prop_DeviceIsWireless_Bool, false);
        vr::VRProperties()->SetBoolProperty(props_, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        vr::VRProperties()->SetFloatProperty(props_, vr::Prop_UserIpdMeters_Float, 0.063f);
        vr::VRProperties()->SetFloatProperty(props_, vr::Prop_DisplayFrequency_Float, 90.0f);
        vr::VRProperties()->SetFloatProperty(props_, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.01111111f);
        vr::VRProperties()->SetBoolProperty(props_, vr::Prop_IsOnDesktop_Bool, false);
        vr::VRProperties()->SetUint64Property(props_, vr::Prop_CurrentUniverseId_Uint64, 2);
        vr::VRProperties()->SetBoolProperty(props_, vr::Prop_HasDriverDirectModeComponent_Bool, true);
        vr::VRProperties()->SetBoolProperty(props_, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, false);
        if (!directMode_.InitializeGraphicsIdentity()) {
            vr::VRDriverLog()->Log("Direct Mode graphics adapter initialization failed");
            return vr::VRInitError_Driver_Failed;
        }
        vr::VRProperties()->SetUint64Property(props_, vr::Prop_GraphicsAdapterLuid_Uint64,
                                              directMode_.AdapterLuid());
        active_ = true;
        vr::VRDriverLog()->Log("Direct Mode HMD activated");
        return vr::VRInitError_None;
    }

    void Deactivate() override { active_ = false; index_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char* componentNameAndVersion) override {
        if (componentNameAndVersion &&
            std::strcmp(componentNameAndVersion, vr::IVRDisplayComponent_Version) == 0) {
            return &display_;
        }
        if (componentNameAndVersion &&
            std::strcmp(componentNameAndVersion, vr::IVRDriverDirectModeComponent_Version) == 0) {
            vr::VRDriverLog()->Log("Direct Mode component queried");
            return static_cast<vr::IVRDriverDirectModeComponent*>(&directMode_);
        }
        return nullptr;
    }
    void DebugRequest(const char*, char* response, uint32_t responseSize) override {
        if (responseSize) response[0] = '\0';
    }
    vr::DriverPose_t GetPose() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return pose_;
    }

    void RunFrame(double dt) {
        if (!active_) return;
        (void)dt;
        bool ipcActive = false;
        double ipcPositionX = positionX_;
        double ipcPositionZ = positionZ_;
        double ipcYaw = yaw_;
        double ipcPitch = pitch_;
        bigscreen_desktop_ipc::ViewMode viewMode = bigscreen_desktop_ipc::ViewMode::Normal;
        if (ReadPoseFromIpc(ipcPositionX, ipcPositionZ, ipcYaw, ipcPitch, ipcActive, viewMode)) {
            positionX_ = ipcPositionX;
            positionZ_ = ipcPositionZ;
            yaw_ = ipcYaw;
            pitch_ = std::clamp(ipcPitch, -static_cast<double>(g_config.pitchLimit),
                                static_cast<double>(g_config.pitchLimit));
        }
        if (viewMode != lastViewMode_) {
            lastViewMode_ = viewMode;
            vr::VRDriverLog()->Log(viewMode == bigscreen_desktop_ipc::ViewMode::Normal
                ? "VIEW DIAGNOSTIC: NORMAL HMD POSE"
                : "VIEW DIAGNOSTIC: LEGACY HMD OFFSET back=2.50m height=0.90m");
        }
        if (ipcActive != ipcConnected_) {
            ipcConnected_ = ipcActive;
            vr::VRDriverLog()->Log(ipcActive ? "Pose IPC connected" : "Pose IPC inactive");
        }

        vr::DriverPose_t next{};
        next.poseTimeOffset = 0.0;
        next.qWorldFromDriverRotation = {1, 0, 0, 0};
        next.qDriverFromHeadRotation = {1, 0, 0, 0};
        double cameraX = positionX_;
        double cameraY = 1.6;
        double cameraZ = positionZ_;
        if (viewMode == bigscreen_desktop_ipc::ViewMode::LegacyHmdOffset)
            ApplyLegacyHmdOffset(yaw_, cameraX, cameraY, cameraZ);
        next.vecPosition[0] = cameraX;
        next.vecPosition[1] = cameraY;
        next.vecPosition[2] = cameraZ;
        next.qRotation = QuaternionFromYawPitch(yaw_, pitch_);
        next.poseIsValid = true;
        next.willDriftInYaw = false;
        next.result = vr::TrackingResult_Running_OK;
        next.deviceIsConnected = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pose_ = next;
        }
        if (g_host) g_host->TrackedDevicePoseUpdated(index_, pose_, sizeof(pose_));

        logAccumulator_ += dt;
        if (logAccumulator_ >= g_config.logInterval) {
            logAccumulator_ = 0.0;
            char line[160];
            sprintf_s(line, "Pose x=%.3f z=%.3f yaw=%.3f pitch=%.3f ipc=%s",
                      positionX_, positionZ_, yaw_, pitch_, ipcConnected_ ? "connected" : "inactive");
            vr::VRDriverLog()->Log(line);
        }
    }

private:
    bool ReadPoseFromIpc(double& positionX, double& positionZ,
                         double& yaw, double& pitch, bool& active,
                         bigscreen_desktop_ipc::ViewMode& viewMode) {
        if (!mapping_) {
            mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_desktop_ipc::kMappingName);
            if (mapping_) {
                sharedState_ = static_cast<const bigscreen_desktop_ipc::PoseState*>(
                    MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(bigscreen_desktop_ipc::PoseState)));
                if (!sharedState_) {
                    CloseHandle(mapping_);
                    mapping_ = nullptr;
                }
            }
        }
        if (sharedState_ && bigscreen_desktop_ipc::Read(sharedState_, positionX, positionZ,
                                                        yaw, pitch, active, viewMode)) return true;
        active = false;
        viewMode = bigscreen_desktop_ipc::ViewMode::Normal;
        return false;
    }

    void ClosePoseIpc() {
        if (sharedState_) UnmapViewOfFile(sharedState_);
        sharedState_ = nullptr;
        if (mapping_) CloseHandle(mapping_);
        mapping_ = nullptr;
    }

    uint32_t index_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t props_ = vr::k_ulInvalidPropertyContainer;
    vr::DriverPose_t pose_{};
    std::mutex mutex_;
    bool active_ = false;
    bool ipcConnected_ = false;
    bigscreen_desktop_ipc::ViewMode lastViewMode_ = bigscreen_desktop_ipc::ViewMode::Normal;
    double positionX_ = 0.0, positionZ_ = 0.0;
    double yaw_ = 0.0, pitch_ = 0.0, logAccumulator_ = 0.0;
    HANDLE mapping_ = nullptr;
    const bigscreen_desktop_ipc::PoseState* sharedState_ = nullptr;
    DirectMode directMode_;
    DisplayComponent display_;
};

class SyntheticRightController final : public vr::ITrackedDeviceServerDriver {
public:
    ~SyntheticRightController() { CloseIpc(); }

    vr::EVRInitError Activate(uint32_t objectId) override {
        index_ = objectId;
        bindings_ = bigscreen_bindings::Load(bigscreen_bindings::SettingsPath());
        props_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_TrackingSystemName_String, "BigscreenDesktopBridge");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ModelNumber_String, "Bigscreen Desktop Right Controller");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ManufacturerName_String, "BigscreenDesktopBridge");
        vr::VRProperties()->SetInt32Property(props_, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_RightHand);
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ControllerType_String, "vive_controller");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_RenderModelName_String, "vr_controller_vive_1_5");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_InputProfilePath_String,
                                              "{htc}/input/vive_controller_profile.json");

        const auto input = vr::VRDriverInput();
        const auto logInput = [](const char* path, vr::EVRInputError error, vr::VRInputComponentHandle_t handle) {
            if (error != vr::VRInputError_None || handle == vr::k_ulInvalidInputComponentHandle) {
                char message[256]{};
                std::snprintf(message, sizeof(message), "Synthetic controller input component failed path=%s error=%d handle=%llu",
                              path, static_cast<int>(error), static_cast<unsigned long long>(handle));
                vr::VRDriverLog()->Log(message);
            }
        };
        auto error = input->CreateBooleanComponent(props_, "/input/trigger/click", &triggerClick_);
        logInput("/input/trigger/click", error, triggerClick_);
        error = input->CreateScalarComponent(props_, "/input/trigger/value", &triggerValue_,
                                              vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        logInput("/input/trigger/value", error, triggerValue_);
        error = input->CreateBooleanComponent(props_, "/input/trigger/touch", &triggerTouch_);
        logInput("/input/trigger/touch", error, triggerTouch_);
        error = input->CreateBooleanComponent(props_, "/input/application_menu/click", &menuClick_);
        logInput("/input/application_menu/click", error, menuClick_);
        error = input->CreateBooleanComponent(props_, "/input/system/click", &systemClick_);
        logInput("/input/system/click", error, systemClick_);
        error = input->CreateScalarComponent(props_, "/input/trackpad/x", &trackpadX_,
                                              vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        logInput("/input/trackpad/x", error, trackpadX_);
        error = input->CreateScalarComponent(props_, "/input/trackpad/y", &trackpadY_,
                                              vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        logInput("/input/trackpad/y", error, trackpadY_);
        error = input->CreateBooleanComponent(props_, "/input/trackpad/click", &trackpadClick_);
        logInput("/input/trackpad/click", error, trackpadClick_);
        error = input->CreateBooleanComponent(props_, "/input/trackpad/touch", &trackpadTouch_);
        logInput("/input/trackpad/touch", error, trackpadTouch_);
        error = input->CreateBooleanComponent(props_, "/input/grip/click", &gripClick_);
        logInput("/input/grip/click", error, gripClick_);
        error = input->CreateHapticComponent(props_, "/output/haptic", &haptic_);
        logInput("/output/haptic", error, haptic_);
        char layout[256]{};
        std::snprintf(layout, sizeof(layout), "Synthetic controller IPC layout size=%zu magic=0x%08X version=%u triggerHandle=%llu",
                      sizeof(bigscreen_desktop_controller_ipc::ControllerState),
                      bigscreen_desktop_controller_ipc::kMagic,
                      bigscreen_desktop_controller_ipc::kVersion,
                      static_cast<unsigned long long>(triggerClick_));
        vr::VRDriverLog()->Log(layout);
        active_ = true;
        vr::VRDriverLog()->Log("Synthetic right controller activated");
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        active_ = false;
        index_ = vr::k_unTrackedDeviceIndexInvalid;
    }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t responseSize) override {
        if (responseSize) response[0] = '\0';
    }
    vr::DriverPose_t GetPose() override { return pose_; }

    void RunFrame(double dt) {
        if (!active_) return;
        (void)dt;
        bigscreen_desktop_controller_ipc::ControllerState controller{};
        const bool inputActive = ReadController(controller);

        double positionX = 0.0;
        double positionZ = 0.0;
        double yaw = 0.0;
        double pitch = 0.0;
        bool hmdActive = false;
        bigscreen_desktop_ipc::ViewMode viewMode = bigscreen_desktop_ipc::ViewMode::Normal;
        ReadHmdPose(positionX, positionZ, yaw, pitch, hmdActive, viewMode);
        const vr::HmdQuaternion_t rotation = QuaternionFromYawPitch(yaw, pitch);
        const float localX = 0.30f + static_cast<float>(armWidthOffset_);
        const float localY = -0.25f + static_cast<float>(armHeightOffset_);
        const float localZ = -0.40f;
        const float rotatedX = static_cast<float>((1.0 - 2.0 * rotation.y * rotation.y - 2.0 * rotation.z * rotation.z) * localX +
            (2.0 * rotation.x * rotation.y - 2.0 * rotation.z * rotation.w) * localY +
            (2.0 * rotation.x * rotation.z + 2.0 * rotation.y * rotation.w) * localZ);
        const float rotatedY = static_cast<float>((2.0 * rotation.x * rotation.y + 2.0 * rotation.z * rotation.w) * localX +
            (1.0 - 2.0 * rotation.x * rotation.x - 2.0 * rotation.z * rotation.z) * localY +
            (2.0 * rotation.y * rotation.z - 2.0 * rotation.x * rotation.w) * localZ);
        const float rotatedZ = static_cast<float>((2.0 * rotation.x * rotation.z - 2.0 * rotation.y * rotation.w) * localX +
            (2.0 * rotation.y * rotation.z + 2.0 * rotation.x * rotation.w) * localY +
            (1.0 - 2.0 * rotation.x * rotation.x - 2.0 * rotation.y * rotation.y) * localZ);

        vr::DriverPose_t next{};
        next.qWorldFromDriverRotation = {1, 0, 0, 0};
        next.qDriverFromHeadRotation = {1, 0, 0, 0};
        double cameraX = positionX;
        double cameraY = 1.6;
        double cameraZ = positionZ;
        if (viewMode == bigscreen_desktop_ipc::ViewMode::LegacyHmdOffset)
            ApplyLegacyHmdOffset(yaw, cameraX, cameraY, cameraZ);
        next.vecPosition[0] = static_cast<float>(cameraX) + rotatedX;
        next.vecPosition[1] = static_cast<float>(cameraY) + rotatedY;
        next.vecPosition[2] = static_cast<float>(cameraZ) + rotatedZ;
        next.qRotation = rotation;
        next.poseIsValid = hmdActive;
        next.result = hmdActive ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
        next.deviceIsConnected = inputActive;
        pose_ = next;
        if (g_host) g_host->TrackedDevicePoseUpdated(index_, pose_, sizeof(pose_));

        const bool a = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_A) != 0;
        const bool b = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_B) != 0;
        const bool x = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_X) != 0;
        const bool y = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_Y) != 0;
        const bool leftBumper = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_LeftBumper) != 0;
        const bool rightBumper = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_RightBumper) != 0;
        const bool leftStick = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_LeftStick) != 0;
        const bool rightStick = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_RightStick) != 0;
        const bool view = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_View) != 0;
        const bool menu = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_Menu) != 0;
        const bool configuredTrigger = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::VRTrigger, controller);
        const bool configuredGrip = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::VRGrip, controller);
        const bool configuredMenu = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::VRMenu, controller);
        const bool configuredTrackpad = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::VRTrackpadClick, controller);
        const bool configuredMic = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::MicToggle, controller);
        const bool configuredArmUp = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::ArmUp, controller);
        const bool configuredArmDown = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::ArmDown, controller);
        const bool configuredArmLeft = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::ArmLeft, controller);
        const bool configuredArmRight = inputActive && ConfiguredGamepadAction(bindings_, bigscreen_bindings::Action::ArmRight, controller);

        // WGI reports D-pad bits, while HID reports the hat switch as 0..7
        // (down=4). Normalize both forms to a Vive trackpad direction.
        float dpadX = 0.0f;
        float dpadY = 0.0f;
        if (controller.dpad & 0x40u) dpadY = 1.0f;
        if (controller.dpad & 0x80u) dpadY = -1.0f;
        if (controller.dpad & 0x100u) dpadX = -1.0f;
        if (controller.dpad & 0x200u) dpadX = 1.0f;
        if (controller.dpad <= 7u) {
            static constexpr float kHatX[] = {0, 1, 1, 1, 0, -1, -1, -1};
            static constexpr float kHatY[] = {1, 1, 0, -1, -1, -1, 0, 1};
            dpadX = kHatX[controller.dpad];
            dpadY = kHatY[controller.dpad];
        }
        if (configuredArmUp || configuredArmDown || configuredArmLeft || configuredArmRight) {
            dpadX = configuredArmLeft ? -1.0f : (configuredArmRight ? 1.0f : 0.0f);
            dpadY = configuredArmUp ? 1.0f : (configuredArmDown ? -1.0f : 0.0f);
        }
        const bool dpadActive = inputActive && (dpadX != 0.0f || dpadY != 0.0f);
        if (controller.dpad != lastDpad_) {
            char dpadMessage[160]{};
            std::snprintf(dpadMessage, sizeof(dpadMessage),
                          "Synthetic controller D-pad raw=0x%08X decoded=(%+.1f,%+.1f) active=%s",
                          controller.dpad, dpadX, dpadY, dpadActive ? "yes" : "no");
            vr::VRDriverLog()->Log(dpadMessage);
            lastDpad_ = controller.dpad;
        }
        const bool rightGrip = inputActive && controller.rightTrigger > 0.5f;
        const bool rightTriggerButton = configuredTrigger || rightBumper;
        if (inputActive && dpadActive) {
            armHeightOffset_ = std::clamp(armHeightOffset_ +
                                          static_cast<double>(dpadY) * kArmAdjustRateMetersPerSecond * dt,
                                          -0.45, 0.65);
            // Left widens the right hand outward; right narrows the spacing.
            armWidthOffset_ = std::clamp(armWidthOffset_ -
                                         static_cast<double>(dpadX) * kArmAdjustRateMetersPerSecond * dt,
                                         -0.20, 0.50);
        }
        // Bigscreen's Vive bindings use the trigger for normal selection, but
        // some menu surfaces consume the trackpad-click action instead. Keep
        // A on the proven trigger path and mirror it to trackpad click for
        // compatibility with those surfaces.
        const bool trackpadPress = configuredTrackpad || a || b || rightStick || leftStick;
        const bool grip = configuredGrip || x || leftBumper || leftStick || rightGrip;
        // Keep Xbox Menu exclusively on SteamVR's system/dashboard action;
        // do not also send it to Bigscreen's in-game application menu.
        const bool applicationMenu = configuredMenu || y || view;
        const float rightTriggerPull = rightTriggerButton ? 1.0f : 0.0f;
        vr::VRDriverInput()->UpdateBooleanComponent(triggerClick_, rightTriggerButton, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(triggerValue_, rightTriggerPull, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(triggerTouch_, rightTriggerButton, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(menuClick_, applicationMenu, 0.0);
        // Xbox Menu (three lines) is the SteamVR system/dashboard toggle.
        vr::VRDriverInput()->UpdateBooleanComponent(systemClick_, menu, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(trackpadX_, 0.0, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(trackpadY_, (configuredMic || leftStick) ? -1.0 : 0.0, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(trackpadClick_, trackpadPress, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(trackpadTouch_, configuredMic || leftStick, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(gripClick_, grip, 0.0);
        if (rightTriggerButton != lastA_) {
            vr::VRDriverLog()->Log(rightTriggerButton ? "Synthetic controller trigger active" : "Synthetic controller trigger released");
            lastA_ = rightTriggerButton;
        }
    }

private:
    bool ReadController(bigscreen_desktop_controller_ipc::ControllerState& out) {
        if (!mapping_) {
            mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_desktop_controller_ipc::kMappingName);
            if (mapping_) {
                state_ = static_cast<const bigscreen_desktop_controller_ipc::ControllerState*>(
                    MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(bigscreen_desktop_controller_ipc::ControllerState)));
                if (!state_) {
                    CloseHandle(mapping_);
                    mapping_ = nullptr;
                }
                else {
                    vr::VRDriverLog()->Log("Synthetic controller IPC mapping opened");
                }
            }
        }
        if (!state_) return false;
        if (!bigscreen_desktop_controller_ipc::Read(state_, out)) return false;
        const bool connected = out.connected != 0;
        const bool a = connected && (out.buttons & bigscreen_desktop_controller_ipc::Button_A) != 0;
        if (a != lastReadA_) {
            char message[256]{};
            std::snprintf(message, sizeof(message), "[A-TRACE] Driver IPC read A=%s connected=%s sequence=%ld",
                          a ? "DOWN" : "UP", connected ? "yes" : "no", out.sequence);
            vr::VRDriverLog()->Log(message);
            lastReadA_ = a;
        }
        return connected;
    }

    void ReadHmdPose(double& positionX, double& positionZ,
                     double& yaw, double& pitch, bool& active,
                     bigscreen_desktop_ipc::ViewMode& viewMode) {
        if (!poseMapping_) {
            poseMapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, bigscreen_desktop_ipc::kMappingName);
            if (poseMapping_) {
                poseState_ = static_cast<const bigscreen_desktop_ipc::PoseState*>(
                    MapViewOfFile(poseMapping_, FILE_MAP_READ, 0, 0, sizeof(bigscreen_desktop_ipc::PoseState)));
                if (!poseState_) {
                    CloseHandle(poseMapping_);
                    poseMapping_ = nullptr;
                }
            }
        }
        if (poseState_ && bigscreen_desktop_ipc::Read(poseState_, positionX, positionZ,
                                                      yaw, pitch, active, viewMode)) return;
        active = false;
        viewMode = bigscreen_desktop_ipc::ViewMode::Normal;
    }

    void CloseIpc() {
        if (state_) UnmapViewOfFile(state_);
        if (mapping_) CloseHandle(mapping_);
        if (poseState_) UnmapViewOfFile(poseState_);
        if (poseMapping_) CloseHandle(poseMapping_);
        state_ = nullptr;
        mapping_ = nullptr;
        poseState_ = nullptr;
        poseMapping_ = nullptr;
    }

    uint32_t index_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t props_ = vr::k_ulInvalidPropertyContainer;
    vr::DriverPose_t pose_{};
    bool active_ = false;
    bool lastA_ = false;
    uint32_t lastDpad_ = 8;
    bool lastReadA_ = false;
    double armHeightOffset_ = 0.0;
    double armWidthOffset_ = 0.0;
    std::map<bigscreen_bindings::Action, bigscreen_bindings::Binding> bindings_;
    vr::VRInputComponentHandle_t triggerClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t triggerValue_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t triggerTouch_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t menuClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t systemClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t trackpadX_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t trackpadY_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t trackpadClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t trackpadTouch_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t gripClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t haptic_ = vr::k_ulInvalidInputComponentHandle;
    HANDLE mapping_ = nullptr;
    const bigscreen_desktop_controller_ipc::ControllerState* state_ = nullptr;
    HANDLE poseMapping_ = nullptr;
    const bigscreen_desktop_ipc::PoseState* poseState_ = nullptr;
};

class Provider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        g_host = vr::VRServerDriverHost();
        g_input = vr::VRDriverInput();
        g_properties = vr::VRProperties();
        vr::VRDriverLog()->Log("Driver initialized");
        hmd_ = new VirtualHmd();
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded("BigscreenDesktopHMD", vr::TrackedDeviceClass_HMD, hmd_)) {
            delete hmd_; hmd_ = nullptr;
            vr::VRDriverLog()->Log("Virtual HMD registration failed");
            return vr::VRInitError_Driver_Failed;
        }
        vr::VRDriverLog()->Log("Virtual HMD registered");
        controller_ = new SyntheticRightController();
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded("BigscreenDesktopRightController",
                                                           vr::TrackedDeviceClass_Controller, controller_)) {
            delete controller_; controller_ = nullptr;
            vr::VRDriverLog()->Log("Synthetic right controller registration failed");
            return vr::VRInitError_Driver_Failed;
        }
        vr::VRDriverLog()->Log("Synthetic right controller registered");
        lastTick_ = GetTickCount64();
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        delete controller_; controller_ = nullptr;
        delete hmd_; hmd_ = nullptr;
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
        g_host = nullptr; g_input = nullptr; g_properties = nullptr;
        vr::VRDriverLog()->Log("Driver shutdown");
    }
    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {
        const ULONGLONG now = GetTickCount64();
        const double dt = std::clamp(static_cast<double>(now - lastTick_) / 1000.0, 0.0, 0.1);
        lastTick_ = now;
        if (hmd_) hmd_->RunFrame(dt);
        if (controller_) controller_->RunFrame(dt);
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
private:
    VirtualHmd* hmd_ = nullptr;
    SyntheticRightController* controller_ = nullptr;
    ULONGLONG lastTick_ = 0;
};

Provider g_provider;
}

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (std::strcmp(vr::IServerTrackedDeviceProvider_Version, interfaceName) == 0) return &g_provider;
    if (returnCode) *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
