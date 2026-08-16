#include <openvr_driver.h>
#include <Windows.h>
#include "controller_ipc.h"
#include "pose_ipc.h"
#include "direct_mode.h"
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
vr::IVRServerDriverHost* g_host = nullptr;
vr::IVRDriverInput* g_input = nullptr;
vr::CVRPropertyHelpers* g_properties = nullptr;

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
    const double cy = std::cos(yaw * 0.5), sy = std::sin(yaw * 0.5);
    const double cp = std::cos(pitch * 0.5), sp = std::sin(pitch * 0.5);
    vr::HmdQuaternion_t q{};
    q.w = cy * cp;
    q.x = sy * sp;
    q.y = sy * cp;
    q.z = cy * sp;
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
        *left = -1.0f; *right = 1.0f; *top = -1.0f; *bottom = 1.0f;
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
        double ipcYaw = yaw_;
        double ipcPitch = pitch_;
        if (ReadPoseFromIpc(ipcYaw, ipcPitch, ipcActive)) {
            yaw_ = ipcYaw;
            pitch_ = std::clamp(ipcPitch, -static_cast<double>(g_config.pitchLimit),
                                static_cast<double>(g_config.pitchLimit));
        }
        if (ipcActive != ipcConnected_) {
            ipcConnected_ = ipcActive;
            vr::VRDriverLog()->Log(ipcActive ? "Pose IPC connected" : "Pose IPC inactive");
        }

        vr::DriverPose_t next{};
        next.poseTimeOffset = 0.0;
        next.qWorldFromDriverRotation = {1, 0, 0, 0};
        next.qDriverFromHeadRotation = {1, 0, 0, 0};
        next.vecPosition[0] = 0.0;
        next.vecPosition[1] = 1.6;
        next.vecPosition[2] = 0.0;
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
            sprintf_s(line, "Pose yaw=%.3f pitch=%.3f ipc=%s", yaw_, pitch_, ipcConnected_ ? "connected" : "inactive");
            vr::VRDriverLog()->Log(line);
        }
    }

private:
    bool ReadPoseFromIpc(double& yaw, double& pitch, bool& active) {
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
        if (sharedState_ && bigscreen_desktop_ipc::Read(sharedState_, yaw, pitch, active)) return true;
        active = false;
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
        props_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_TrackingSystemName_String, "BigscreenDesktopBridge");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ModelNumber_String, "Bigscreen Desktop Right Controller");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ManufacturerName_String, "BigscreenDesktopBridge");
        vr::VRProperties()->SetInt32Property(props_, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_RightHand);
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_ControllerType_String, "vive_controller");
        vr::VRProperties()->SetStringProperty(props_, vr::Prop_InputProfilePath_String,
                                              "{htc}/input/vive_controller_profile.json");

        vr::VRDriverInput()->CreateBooleanComponent(props_, "/input/trigger/click", &triggerClick_);
        vr::VRDriverInput()->CreateScalarComponent(props_, "/input/trigger/value", &triggerValue_,
                                                   vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        vr::VRDriverInput()->CreateBooleanComponent(props_, "/input/trigger/touch", &triggerTouch_);
        vr::VRDriverInput()->CreateBooleanComponent(props_, "/input/application_menu/click", &menuClick_);
        vr::VRDriverInput()->CreateScalarComponent(props_, "/input/trackpad/x", &trackpadX_,
                                                   vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        vr::VRDriverInput()->CreateScalarComponent(props_, "/input/trackpad/y", &trackpadY_,
                                                   vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        vr::VRDriverInput()->CreateBooleanComponent(props_, "/input/trackpad/click", &trackpadClick_);
        vr::VRDriverInput()->CreateHapticComponent(props_, "/output/haptic", &haptic_);
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

        double yaw = 0.0;
        double pitch = 0.0;
        bool hmdActive = false;
        ReadHmdPose(yaw, pitch, hmdActive);
        const vr::HmdQuaternion_t rotation = QuaternionFromYawPitch(yaw, pitch);
        const float localX = 0.30f;
        const float localY = -0.25f;
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
        next.vecPosition[0] = rotatedX;
        next.vecPosition[1] = 1.6f + rotatedY;
        next.vecPosition[2] = rotatedZ;
        next.qRotation = rotation;
        next.poseIsValid = hmdActive;
        next.result = hmdActive ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
        next.deviceIsConnected = inputActive;
        pose_ = next;
        if (g_host) g_host->TrackedDevicePoseUpdated(index_, pose_, sizeof(pose_));

        const bool a = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_A) != 0;
        const bool b = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_B) != 0;
        const bool rightStick = inputActive && (controller.buttons & bigscreen_desktop_controller_ipc::Button_RightStick) != 0;
        vr::VRDriverInput()->UpdateBooleanComponent(triggerClick_, a, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(triggerValue_, a ? 1.0 : 0.0, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(triggerTouch_, a, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(menuClick_, b, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(trackpadX_, 0.0, 0.0);
        vr::VRDriverInput()->UpdateScalarComponent(trackpadY_, 0.0, 0.0);
        vr::VRDriverInput()->UpdateBooleanComponent(trackpadClick_, rightStick, 0.0);
        if (a != lastA_) {
            vr::VRDriverLog()->Log(a ? "Synthetic controller trigger active" : "Synthetic controller trigger released");
            lastA_ = a;
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
            }
        }
        if (!state_ || !bigscreen_desktop_controller_ipc::Read(state_, out)) return false;
        return out.connected != 0;
    }

    void ReadHmdPose(double& yaw, double& pitch, bool& active) {
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
        if (poseState_ && bigscreen_desktop_ipc::Read(poseState_, yaw, pitch, active)) return;
        active = false;
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
    vr::VRInputComponentHandle_t triggerClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t triggerValue_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t triggerTouch_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t menuClick_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t trackpadX_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t trackpadY_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t trackpadClick_ = vr::k_ulInvalidInputComponentHandle;
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
