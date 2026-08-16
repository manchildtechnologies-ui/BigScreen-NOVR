# Bigscreen Desktop Bridge

Use Bigscreen on a Windows monitor without a physical VR headset. This project
provides a synthetic SteamVR HMD, Xbox controller input, a synthetic right-hand
controller, and a desktop viewer for the live Bigscreen frame.

## Download for friends

Download the ZIP from the repository's **Releases** page, extract it, and run
`BigscreenDesktopInstaller.exe`. The installer creates **Start Bigscreen
Desktop** and **Uninstall Bigscreen Desktop Bridge** shortcuts.

## Requirements

- Windows 10/11 x64
- SteamVR and Bigscreen installed separately
- Bluetooth Xbox Wireless Controller paired in Windows
- DirectX 11-capable graphics driver

No physical VR headset or separate Visual C++ runtime is required. The custom
driver is installed in SteamVR's protected directory, so Windows asks for
administrator permission during installation.

## Use

1. Run **Start Bigscreen Desktop**.
2. Wait until SteamVR and the bridge are ready.
3. Start Bigscreen normally through Steam.
4. Click **Open Viewer** in the launcher.

The bridge runs silently in the background. The viewer opens from the launcher.

## Xbox mappings

- Right stick: look
- Left stick: walk and strafe
- A: Vive trigger / primary click
- Right trigger: analog trigger pull
- B: Vive trackpad / secondary click
- D-pad: trackpad direction; Down toggles microphone
- X, bumpers, left-stick click: Vive grip
- Y, View, Menu: Vive application/menu
- Right-stick click: trackpad click

## Uninstall

Double-click **Uninstall Bigscreen Desktop Bridge**. It removes the bridge,
viewer, launcher, shortcuts, and custom SteamVR driver. Bigscreen and SteamVR
are left installed.

## Building from source

Use Visual Studio Build Tools with C++ support, CMake, and an x64 generator.
Set `OPENVR_SDK_ROOT` to an official Valve OpenVR checkout containing
`headers/openvr_driver.h`:

```powershell
cmake -S . -B build -A x64 -DOPENVR_SDK_ROOT=C:\path\to\openvr
cmake --build build --config Release
```

Official OpenVR repository: https://github.com/ValveSoftware/openvr

## Scope and licensing

This repository contains the bridge and driver prototype only. It does not
include Bigscreen, SteamVR, or Valve driver files. Add the project's chosen
open-source license before publishing publicly.
