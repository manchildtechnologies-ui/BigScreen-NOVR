BIGSCREEN DESKTOP BRIDGE
========================

Use Bigscreen on a normal Windows monitor without a physical VR headset.
This package creates a synthetic SteamVR HMD, reads an Xbox controller, and
shows the live Bigscreen view on the desktop.

WHAT IS INCLUDED
----------------

BigscreenDesktopInstaller.exe
    Graphical installer. It uses the included Bigscreen artwork and creates
    the desktop shortcut named Start Bigscreen Desktop.

BigscreenDesktopLauncher.exe
    The normal program to use after installation. It starts SteamVR, starts
    the bridge silently in the background, and provides an Open Viewer button.

BigscreenDesktopBridge.exe
    Background input and pose bridge. It reads the Bluetooth Xbox controller,
    updates the synthetic HMD position/orientation, and sends controller input
    to SteamVR.

BigscreenDesktopViewer.exe
    Desktop window that displays the live GPU frame from Bigscreen.

BigscreenDesktopUninstaller.exe
    Removes the installed bridge, viewer, launcher, desktop shortcuts, and
    custom SteamVR driver without removing Bigscreen or SteamVR.

steamvr_driver\
    The custom SteamVR driver containing the synthetic HMD and right-hand
    controller.

INSTALLATION
------------

1. Extract the ZIP file.
2. Open the BigscreenDesktopBridge-x64 folder.
3. Double-click BigscreenDesktopInstaller.exe.
4. Approve the normal Windows permission prompt.

The installer places the applications in your local user profile, installs
the custom driver into SteamVR, and creates a Start Bigscreen Desktop shortcut.

UNINSTALLATION
--------------

Double-click the desktop shortcut **Uninstall Bigscreen Desktop Bridge** and
confirm the prompt. It stops only the bridge, viewer, and launcher, removes
the custom SteamVR driver, and removes the installed files and shortcuts.
Bigscreen and SteamVR are left installed.

WHY WINDOWS ASKS FOR PERMISSION
-------------------------------

SteamVR drivers are installed under the protected Program Files folder. Windows
requires administrator permission to copy the custom driver there. The bridge
and viewer are installed in the user profile and do not need administrator
permission. The installer elevates only when necessary.

REQUIREMENTS
------------

- Windows 10 or Windows 11, 64-bit
- Steam installed with SteamVR
- Bigscreen installed separately through Steam
- Bluetooth Xbox Wireless Controller paired in Windows
- A working graphics driver supporting DirectX 11

No physical VR headset is required.
No separate Visual C++ runtime installation should be required; the included
programs use the static MSVC runtime.

STARTING BIGSCREEN
-------------------

1. Double-click Start Bigscreen Desktop.
2. Wait for the launcher to say SteamVR and the bridge are ready.
3. Start Bigscreen normally through Steam.
4. When Bigscreen is running, click Open Viewer in the launcher.

The bridge runs silently in the background. The viewer opens only when you
press Open Viewer.

XBOX CONTROLLER MAPPINGS
------------------------

Right stick
    Look left, right, up, and down.

Left stick
    Walk forward/backward and strafe left/right.

Right-stick click
    Vive-style trackpad click.

A
    Vive trigger / primary click.

Right trigger
    Analog trigger pull.

B
    Vive trackpad click / secondary button.

D-pad
    Vive trackpad direction.

D-pad Down
    Bigscreen microphone toggle / push-to-talk action.

X, left/right bumpers, left-stick click
    Vive grip action.

Y, View, Menu
    Vive application/menu action.

Keyboard controls remain available in the bridge window. R recenters the
synthetic HMD orientation. F8 enables mouse-look and Escape releases it.

TROUBLESHOOTING
---------------

SteamVR does not start
    Start SteamVR once manually, close it, and run Start Bigscreen Desktop
    again. Confirm SteamVR is installed in the normal Steam library.

The Xbox controller is not detected
    Confirm it is powered on and visible in Windows. Bluetooth pairing and
    controller health are outside this package.

The viewer does not open
    Start Bigscreen first, wait until it is rendering, then press Open Viewer.

The installer does not launch
    Make sure you double-click BigscreenDesktopInstaller.exe inside the
    extracted folder, not the ZIP file or the folder itself. Right-click Run
    as administrator is an optional fallback.

The desktop shortcut is missing
    Run the installer again and approve the Windows permission prompt.

IMPORTANT
---------

This package does not include Bigscreen, SteamVR, Valve driver files, or
controller-pairing software. Those are installed separately through Steam or
Windows. Do not copy or modify Bigscreen's own files.
