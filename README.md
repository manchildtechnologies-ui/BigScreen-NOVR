# Bigscreen NOVR




<img width="1276" height="927" alt="image" src="https://github.com/user-attachments/assets/beac986a-f5f7-4776-ab87-355679c35294" />

<img width="1275" height="933" alt="image" src="https://github.com/user-attachments/assets/718d176a-c965-48b6-951a-222a1d5d0d92" />


Bigscreen NOVR is a community desktop viewer and control bridge for Bigscreen. This build provides a keyboard-and-Xbox-controller Controls page, first-person and third-person viewing modes, and a native bridge for forwarding desktop controller input into the runtime.

## Current build

- Tauri desktop application with a Windows NSIS installer.
- The installer registers a standard Windows uninstaller through Add/Remove Programs.
- Native bridge and third-person runtime components are bundled with the application.
- Controls page documents the active keyboard and Xbox controller bindings.
- Controller commands are shared between the desktop UI and native runtime through the `Local\\BigscreenDesktopControllerControl` mapping.

## Xbox controller bindings

| Control | Action |
| --- | --- |
| A | First Person |
| B | Third Person |
| X | Toggle virtual controllers in First Person |
| Y | Reset view |
| Left Stick | Move / strafe in Third Person |
| Left Stick Click | Sprint while moving in Third Person |
| Right Stick | Look / rotate camera in Third Person |
| Right Stick Click | Recenter camera in Third Person |
| LT / RT | Zoom out / zoom in in Third Person |
| LB / RB | Previous / next camera in Third Person |
| D-pad Up / Down | Increase / decrease camera height |
| D-pad Left / Right | Decrease / increase field of view |

## Keyboard bindings

- `1` — First Person
- `3` — Third Person
- `R` — Reset view
- Arrow keys — Adjust camera pitch and yaw
- `F6` — Toggle virtual controllers
- `Esc` — Close / go back




## Build the Windows application

From the `ui` directory:

```powershell
npm install
npm run tauri build -- --debug
```

The NSIS installer is generated under:

```text
ui/src-tauri/target/debug/bundle/nsis/
```

The native runtime must be built into `build/native-frida/runtime` before bundling so the bridge, viewer, host, GPU export, OpenVR, and Frida runtime assets are included.

## Repository notes

Generated dependency, frontend, and Tauri target directories are intentionally ignored. Native and Frida license notices are included under `licenses/` and are redistributed with the runtime bundle where required.

This is an unofficial community project and is not affiliated with Bigscreen.
