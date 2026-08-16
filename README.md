# BigscreenDesktopBridge — Phase 7

This is an isolated, reversible OpenVR server-driver prototype. It does not modify Bigscreen, SteamVR binaries, `driver_null`, or any Bigscreen files.

## Current build status

The project builds as an x64 Release DLL with Visual Studio 2019 Build Tools, Windows SDK 10.0.19041.0, and CMake 3.20.21032501-MSVC_2. The local Valve OpenVR archive reports SDK version 2.15.6 in `headers/openvr.h`; its ZIP extraction has no commit metadata.

The driver has not been registered with SteamVR and Bigscreen has not been launched for this prototype.

## API target

The driver targets Valve's current OpenVR server-driver contract: `IServerTrackedDeviceProvider`, `ITrackedDeviceServerDriver`, `TrackedDevicePoseUpdated()`, and the exported `HmdDriverFactory`. The SDK header is supplied from the official Valve OpenVR checkout at `C:\Users\Vintendo\Documents\openvr-master` via `OPENVR_SDK_ROOT`.

## Intended build

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64 `
  -DOPENVR_SDK_ROOT="C:\Users\Vintendo\Documents\openvr-master"
cmake --build build --config Release
```

The staged driver is emitted under `build\BigscreenDesktopDriver`.

## Registration safety

Before registration, record:

```powershell
& "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" show
```

Register only the staged directory:

```powershell
& "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" adddriver "C:\path\to\build\BigscreenDesktopDriver"
```

Remove it with:

```powershell
& "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" removedriver "C:\path\to\build\BigscreenDesktopDriver"
```

The active SteamVR settings currently force Valve's `null` driver. That setting must be backed up before any test that changes it. Do not launch Bigscreen until SteamVR-only validation succeeds.
