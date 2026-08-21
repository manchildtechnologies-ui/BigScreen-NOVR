#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::{
    env, fs,
    collections::BTreeMap,
    path::{Path, PathBuf},
    process::Command,
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};
#[cfg(target_os = "windows")]
use std::os::windows::process::CommandExt;

const CREATE_NO_WINDOW: u32 = 0x08000000;

const STEAMVR_APP_ID: &str = "250820";
const BIGSCREEN_APP_ID: &str = "457550";
const WM_KEYDOWN: u32 = 0x0100;
const WM_KEYUP: u32 = 0x0101;
const HWND_TOPMOST: isize = -1;
const HWND_NOTOPMOST: isize = -2;
const SWP_NOMOVE: u32 = 0x0002;
const SWP_NOSIZE: u32 = 0x0001;
const SWP_NOACTIVATE: u32 = 0x0010;
const FILE_MAP_READ: u32 = 0x0004;

#[derive(Clone, Serialize, Deserialize)]
struct SettingState {
    launch_steamvr: bool,
    launch_bigscreen: bool,
    keep_viewer_top: bool,
    start_view: String,
}

#[derive(Clone, Serialize, Deserialize)]
struct BindingValue {
    device: String,
    code: i32,
}

#[derive(Clone, Serialize, Deserialize)]
struct BindingFile {
    #[serde(default = "binding_version")]
    bindings_version: u32,
    bindings: BTreeMap<String, BindingValue>,
}

fn binding_version() -> u32 { 1 }

#[derive(Clone, Serialize)]
struct ControllerSnapshot {
    connected: bool,
    buttons: u32,
    dpad: u32,
}

#[derive(Clone, Serialize)]
struct ControllerCommand {
    view_request: u32,
    reset_epoch: u32,
    virtual_controllers_enabled: bool,
    camera_index: i32,
    camera_distance: f32,
    camera_height: f32,
    camera_fov: f32,
}

#[derive(Default)]
struct OwnedProcesses {
    bridge: bool,
    viewer: bool,
    bigscreen: bool,
    host_pid: Option<u32>,
}
struct RuntimeState(Mutex<OwnedProcesses>);

#[derive(Clone, Serialize)]
struct StatusRow {
    label: String,
    value: String,
    tone: String,
    icon: String,
}

fn settings_path() -> PathBuf {
    dirs::data_local_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("BigscreenNOVR")
        .join("react-settings.json")
}
fn bindings_path() -> PathBuf {
    dirs::data_local_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("BigscreenDesktopNoVR")
        .join("bindings.json")
}
fn default_bindings() -> BindingFile {
    let mut bindings = BTreeMap::new();
    let add = |bindings: &mut BTreeMap<String, BindingValue>, name: &str, device: &str, code: i32| {
        bindings.insert(name.into(), BindingValue { device: device.into(), code });
    };
    add(&mut bindings, "Move Forward", "Keyboard", 38);
    add(&mut bindings, "Move Backward", "Keyboard", 40);
    add(&mut bindings, "Move Left", "Keyboard", 37);
    add(&mut bindings, "Move Right", "Keyboard", 39);
    add(&mut bindings, "Mouse Look Toggle", "Keyboard", 119);
    add(&mut bindings, "Recenter", "Keyboard", 82);
    add(&mut bindings, "Release / Exit", "Keyboard", 27);
    add(&mut bindings, "VR Trigger", "Gamepad", 0);
    add(&mut bindings, "VR Grip", "Gamepad", 5);
    add(&mut bindings, "VR Menu", "Gamepad", 8);
    add(&mut bindings, "VR Trackpad Click", "Gamepad", 7);
    add(&mut bindings, "Mic Toggle", "Gamepad", 6);
    add(&mut bindings, "Arm Up", "Gamepad", 0x40);
    add(&mut bindings, "Arm Down", "Gamepad", 0x80);
    add(&mut bindings, "Arm Left", "Gamepad", 0x100);
    add(&mut bindings, "Arm Right", "Gamepad", 0x200);
    BindingFile { bindings_version: 1, bindings }
}
fn load_bindings() -> BindingFile {
    let defaults = default_bindings();
    let Ok(raw) = fs::read_to_string(bindings_path()) else { return defaults; };
    let Ok(saved) = serde_json::from_str::<BindingFile>(&raw) else { return defaults; };
    let mut merged = defaults.bindings;
    for (name, value) in saved.bindings {
        if merged.contains_key(&name) && (value.device == "Keyboard" || value.device == "Gamepad" || value.device == "None") {
            merged.insert(name, value);
        }
    }
    BindingFile { bindings_version: 1, bindings: merged }
}
fn save_bindings_file(file: &BindingFile) -> Result<(), String> {
    let path = bindings_path();
    if let Some(parent) = path.parent() { fs::create_dir_all(parent).map_err(|e| e.to_string())?; }
    let mut out = String::from("{\n  \"bindingsVersion\": 1,\n  \"bindings\": {\n");
    for (index, (name, value)) in file.bindings.iter().enumerate() {
        if index > 0 { out.push_str(",\n"); }
        out.push_str(&format!("    \"{}\": {{ \"device\": \"{}\", \"code\": {} }}", name, value.device, value.code));
    }
    out.push_str("\n  }\n}\n");
    fs::write(path, out).map_err(|e| e.to_string())
}
fn log_path() -> PathBuf {
    settings_path().parent().unwrap().join("runtime.log")
}
fn log_event(message: &str) {
    if let Some(parent) = log_path().parent() {
        let _ = fs::create_dir_all(parent);
    }
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let line = format!("{now} {message}\n");
    if let Ok(mut file) = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path())
    {
        use std::io::Write;
        let _ = file.write_all(line.as_bytes());
    }
}
fn process_running(name: &str) -> bool {
    Command::new("tasklist")
        .creation_flags(CREATE_NO_WINDOW)
        .args(["/FI", &format!("IMAGENAME eq {name}")])
        .output()
        .map(|o| {
            String::from_utf8_lossy(&o.stdout)
                .to_lowercase()
                .contains(&name.to_lowercase())
        })
        .unwrap_or(false)
}
fn find_pid(name: &str) -> Option<String> {
    let out = Command::new("tasklist")
        .creation_flags(CREATE_NO_WINDOW)
        .args(["/FI", &format!("IMAGENAME eq {name}"), "/FO", "CSV", "/NH"])
        .output()
        .ok()?;
    let line = String::from_utf8_lossy(&out.stdout)
        .lines()
        .find(|l| l.contains(name))?
        .to_string();
    line.split(',')
        .nth(1)
        .map(|p| p.trim_matches('"').to_string())
}
fn runtime_candidates() -> Vec<PathBuf> {
    let mut roots = Vec::new();
    if let Ok(root) = env::var("BIGSCREEN_NOVR_RUNTIME") {
        roots.push(PathBuf::from(root));
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            roots.extend([
                dir.to_path_buf(),
                dir.join("runtime"),
                dir.join("resources"),
                dir.join("resources").join("runtime"),
            ]);
        }
    }
    roots
}
fn runtime_exe(name: &str) -> Option<PathBuf> {
    runtime_candidates()
        .into_iter()
        .map(|p| p.join(name))
        .find(|p| p.is_file())
}
fn spawn_direct(path: &Path) -> Result<(), String> {
    Command::new(path)
        .current_dir(path.parent().unwrap_or(Path::new(".")))
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("Could not start {}: {e}", path.display()))
}
fn spawn_owned(path: &Path) -> Result<u32, String> {
    Command::new(path)
        .current_dir(path.parent().unwrap_or(Path::new(".")))
        .spawn()
        .map(|child| child.id())
        .map_err(|e| format!("Could not start {}: {e}", path.display()))
}
fn open_steam_app(app_id: &str) -> Result<(), String> {
    Command::new("explorer.exe")
        .creation_flags(CREATE_NO_WINDOW)
        .arg(format!("steam://run/{app_id}"))
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}
fn third_status_path() -> PathBuf {
    settings_path().parent().unwrap().join("third-person.status")
}
fn third_status() -> String {
    fs::read_to_string(third_status_path())
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "Offline".into())
}
fn start_third_person_host() -> Result<u32, String> {
    if !process_running("Bigscreen.exe") { return Err("Third-person camera waiting for Bigscreen".into()); }
    let host = runtime_exe("BigscreenThirdPersonHost.exe")
        .ok_or("Third-person camera unavailable")?;
    let pid = spawn_owned(&host)?;
    log_event("third_person_host_started");
    Ok(pid)
}
fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}
fn viewer_hwnd() -> Option<isize> {
    let title = wide("BigscreenDesktopViewer");
    let hwnd = unsafe { FindWindowW(std::ptr::null(), title.as_ptr()) };
    if hwnd == 0 { None } else { Some(hwnd) }
}
fn set_view(mode: &str) -> Result<(), String> {
    let key = if mode == "third" { b'3' } else { b'1' } as usize;
    if let Some(hwnd) = viewer_hwnd() {
        unsafe {
            PostMessageW(hwnd, WM_KEYDOWN, key, 0);
            PostMessageW(hwnd, WM_KEYUP, key, 0);
        }
        log_event(if mode == "third" {
            "view_switch third"
        } else {
            "view_switch first"
        });
        Ok(())
    } else {
        Err("Desktop Viewer is not running".into())
    }
}
fn set_topmost(keep: bool) {
    if let Some(hwnd) = viewer_hwnd() {
        unsafe {
            SetWindowPos(
                hwnd,
                if keep { HWND_TOPMOST } else { HWND_NOTOPMOST },
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
            );
        }
    }
}
fn status(label: &str, value: &str, tone: &str, icon: &str) -> StatusRow {
    StatusRow {
        label: label.into(),
        value: value.into(),
        tone: tone.into(),
        icon: icon.into(),
    }
}
fn camera_status() -> (String, &'static str) {
    let host_running = process_running("BigscreenThirdPersonHost.exe");
    if !host_running {
        return ("Offline".into(), "offline");
    }
    match third_status().as_str() {
        "Running" => ("Running".into(), "ready"),
        "Starting" => ("Starting".into(), "waiting"),
        "Unavailable" => ("Unavailable".into(), "offline"),
        "Error" => ("Error".into(), "offline"),
        _ => ("Offline".into(), "offline"),
    }
}

#[tauri::command]
fn get_system_status() -> Vec<StatusRow> {
    let steam = process_running("vrmonitor.exe") || process_running("vrcompositor.exe");
    let bigscreen = process_running("Bigscreen.exe");
    let bridge = process_running("BigscreenDesktopBridge.exe");
    let viewer = process_running("BigscreenDesktopViewer.exe");
    let (camera, camera_tone) = camera_status();
    vec![
        status(
            "SteamVR",
            if steam { "Running" } else { "Offline" },
            if steam { "ready" } else { "offline" },
            "steam",
        ),
        status(
            "Bigscreen",
            if bigscreen { "Running" } else { "Offline" },
            if bigscreen { "ready" } else { "offline" },
            "bigscreen",
        ),
        status(
            "NOVR Bridge",
            if bridge { "Running" } else { "Offline" },
            if bridge { "ready" } else { "offline" },
            "bridge",
        ),
        status(
            "Desktop Viewer",
            if viewer { "Running" } else { "Offline" },
            if viewer { "ready" } else { "offline" },
            "viewer",
        ),
        status("Third-Person Camera", &camera, camera_tone, "camera"),
    ]
}
#[tauri::command]
fn get_settings() -> SettingState {
    fs::read_to_string(settings_path())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(SettingState {
            launch_steamvr: true,
            launch_bigscreen: false,
            keep_viewer_top: true,
            start_view: "first".into(),
        })
}

#[tauri::command]
fn get_bindings() -> BindingFile { load_bindings() }

#[tauri::command]
fn save_bindings(bindings: BindingFile) -> Result<BindingFile, String> {
    let defaults = default_bindings();
    if bindings.bindings.keys().any(|name| !defaults.bindings.contains_key(name)) {
        return Err("Unknown binding action".into());
    }
    if bindings.bindings.values().any(|value| value.device != "Keyboard" && value.device != "Gamepad" && value.device != "None") {
        return Err("Unsupported binding device".into());
    }
    save_bindings_file(&bindings)?;
    Ok(load_bindings())
}

#[cfg(target_os = "windows")]
fn controller_snapshot() -> ControllerSnapshot {
    use std::mem::size_of;
    let name: Vec<u16> = "Local\\BigscreenDesktopControllerInput".encode_utf16().chain(std::iter::once(0)).collect();
    unsafe {
        let mapping = OpenFileMappingW(FILE_MAP_READ, 0, name.as_ptr());
        if mapping == 0 { return ControllerSnapshot { connected: false, buttons: 0, dpad: 8 }; }
        let ptr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size_of::<[u8; 40]>());
        if ptr.is_null() { CloseHandle(mapping); return ControllerSnapshot { connected: false, buttons: 0, dpad: 8 }; }
        let bytes = std::slice::from_raw_parts(ptr as *const u8, 40);
        let connected = u32::from_ne_bytes(bytes[12..16].try_into().unwrap()) != 0;
        let buttons = u32::from_ne_bytes(bytes[40..44].try_into().unwrap());
        let dpad = u32::from_ne_bytes(bytes[44..48].try_into().unwrap());
        UnmapViewOfFile(ptr);
        CloseHandle(mapping);
        ControllerSnapshot { connected, buttons, dpad }
    }
}

#[cfg(not(target_os = "windows"))]
fn controller_snapshot() -> ControllerSnapshot {
    ControllerSnapshot { connected: false, buttons: 0, dpad: 8 }
}

#[tauri::command]
fn get_controller_status() -> ControllerSnapshot { controller_snapshot() }

#[cfg(target_os = "windows")]
fn controller_command() -> ControllerCommand {
    use std::mem::size_of;
    let name: Vec<u16> = "Local\\BigscreenDesktopControllerControl".encode_utf16().chain(std::iter::once(0)).collect();
    unsafe {
        let mapping = OpenFileMappingW(FILE_MAP_READ, 0, name.as_ptr());
        if mapping == 0 { return ControllerCommand { view_request: 0, reset_epoch: 0, virtual_controllers_enabled: true, camera_index: 0, camera_distance: 2.5, camera_height: 0.6, camera_fov: 60.0 }; }
        let ptr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size_of::<[u8; 48]>());
        if ptr.is_null() { CloseHandle(mapping); return ControllerCommand { view_request: 0, reset_epoch: 0, virtual_controllers_enabled: true, camera_index: 0, camera_distance: 2.5, camera_height: 0.6, camera_fov: 60.0 }; }
        let bytes = std::slice::from_raw_parts(ptr as *const u8, 48);
        let u32_at = |offset: usize| u32::from_ne_bytes(bytes[offset..offset + 4].try_into().unwrap());
        let f32_at = |offset: usize| f32::from_ne_bytes(bytes[offset..offset + 4].try_into().unwrap());
        let result = ControllerCommand {
            view_request: u32_at(12),
            reset_epoch: u32_at(16),
            virtual_controllers_enabled: u32_at(20) != 0,
            camera_index: i32::from_ne_bytes(bytes[24..28].try_into().unwrap()),
            camera_distance: f32_at(28),
            camera_height: f32_at(32),
            camera_fov: f32_at(36),
        };
        UnmapViewOfFile(ptr);
        CloseHandle(mapping);
        result
    }
}

#[cfg(not(target_os = "windows"))]
fn controller_command() -> ControllerCommand {
    ControllerCommand { view_request: 0, reset_epoch: 0, virtual_controllers_enabled: true, camera_index: 0, camera_distance: 2.5, camera_height: 0.6, camera_fov: 60.0 }
}

#[tauri::command]
fn get_controller_command() -> ControllerCommand { controller_command() }
#[tauri::command]
fn save_settings(settings: SettingState) -> SettingState {
    let p = settings_path();
    if let Some(parent) = p.parent() {
        let _ = fs::create_dir_all(parent);
    }
    if let Ok(data) = serde_json::to_vec_pretty(&settings) {
        let _ = fs::write(p, data);
    }
    set_topmost(settings.keep_viewer_top);
    settings
}

#[tauri::command]
fn start_novr(state: tauri::State<'_, RuntimeState>) -> Result<String, String> {
    let settings = get_settings();
    let mut owned = state
        .0
        .lock()
        .map_err(|_| "Runtime state unavailable".to_string())?;
    if process_running("BigscreenDesktopBridge.exe")
        && process_running("BigscreenDesktopViewer.exe")
    {
        let _ = set_view(&settings.start_view);
        return Ok("RUNNING".into());
    }
    log_event("startup_attempt");
    if settings.launch_steamvr && !process_running("vrmonitor.exe") {
        let _ = open_steam_app(STEAMVR_APP_ID);
        log_event("steamvr_launch_requested");
    }
    if !process_running("BigscreenDesktopBridge.exe") {
        let path = runtime_exe("BigscreenDesktopBridge.exe").ok_or(
            "NOVR Bridge unavailable: set BIGSCREEN_NOVR_RUNTIME to the native runtime folder",
        )?;
        spawn_direct(&path)?;
        owned.bridge = true;
        log_event("bridge_started");
    }
    if settings.launch_bigscreen && !process_running("Bigscreen.exe") {
        open_steam_app(BIGSCREEN_APP_ID)?;
        owned.bigscreen = true;
        log_event("bigscreen_launch_requested");
    }
    if !process_running("BigscreenDesktopViewer.exe") {
        let path = runtime_exe("BigscreenDesktopViewer.exe").ok_or(
            "Desktop Viewer unavailable: set BIGSCREEN_NOVR_RUNTIME to the native runtime folder",
        )?;
        spawn_direct(&path)?;
        owned.viewer = true;
        log_event("viewer_started");
    }
    if settings.start_view == "third" && process_running("Bigscreen.exe") && owned.host_pid.is_none() { owned.host_pid = Some(start_third_person_host()?); }
    drop(owned);
    let _ = set_view(&settings.start_view);
    set_topmost(settings.keep_viewer_top);
    Ok("STARTING".into())
}
#[tauri::command]
fn stop_novr(state: tauri::State<'_, RuntimeState>) -> Result<String, String> {
    let mut owned = state
        .0
        .lock()
        .map_err(|_| "Runtime state unavailable".to_string())?;
    for (name, ours) in [
        ("BigscreenDesktopViewer.exe", owned.viewer),
        ("BigscreenDesktopBridge.exe", owned.bridge),
    ] {
        if ours && process_running(name) {
            if let Some(pid) = find_pid(name) {
                let _ = Command::new("taskkill")
                    .creation_flags(CREATE_NO_WINDOW)
                    .args(["/PID", &pid, "/T", "/F"])
                    .output();
            }
            log_event(if name.contains("Viewer") {
                "viewer_stopped"
            } else {
                "bridge_stopped"
            });
        }
    }
    if let Some(pid) = owned.host_pid { let _ = Command::new("taskkill").creation_flags(CREATE_NO_WINDOW).args(["/PID", &pid.to_string(), "/T", "/F"]).output(); log_event("third_person_host_stopped"); }
    if owned.bigscreen {
        log_event("bigscreen_not_terminated_by_design");
    }
    *owned = OwnedProcesses::default();
    Ok("STOPPED".into())
}
#[tauri::command]
fn set_view_mode(mode: String, state: tauri::State<'_, RuntimeState>) -> Result<String, String> {
    if mode != "first" && mode != "third" {
        return Err("Unknown view mode".into());
    }
    if mode == "third" {
        let mut owned = state.0.lock().map_err(|_| "Runtime state unavailable".to_string())?;
        if owned.host_pid.is_none() {
            owned.host_pid = Some(start_third_person_host()?);
        }
        drop(owned);
        for _ in 0..100 {
            match third_status().as_str() {
                "Running" => break,
                "Unavailable" | "Error" => {
                    let _ = set_view("first");
                    return Ok("Third-person camera unavailable".into());
                }
                _ => std::thread::sleep(std::time::Duration::from_millis(100)),
            }
        }
        if third_status() != "Running" {
            let _ = set_view("first");
            return Ok("Third-person camera unavailable".into());
        }
    }
    match set_view(&mode) {
        Ok(()) => Ok(mode),
        Err(e) if e == "Desktop Viewer is not running" => Ok(format!("OFFLINE — {e}")),
        Err(e) => Err(e),
    }
}
#[tauri::command]
fn open_viewer() -> Result<String, String> {
    Command::new("explorer.exe")
        .creation_flags(CREATE_NO_WINDOW)
        .arg(log_path().parent().unwrap())
        .spawn()
        .map_err(|e| e.to_string())?;
    Ok("OPENED".into())
}
#[tauri::command]
fn minimize_window(window: tauri::Window) {
    let _ = window.minimize();
}
#[tauri::command]
fn close_window(window: tauri::Window) {
    let _ = window.close();
}

#[cfg(target_os = "windows")]
#[link(name = "user32")]
extern "system" {
    fn FindWindowW(class_name: *const u16, window_name: *const u16) -> isize;
    fn PostMessageW(hwnd: isize, message: u32, wparam: usize, lparam: isize) -> i32;
    fn SetWindowPos(
        hwnd: isize,
        insert_after: isize,
        x: i32,
        y: i32,
        cx: i32,
        cy: i32,
        flags: u32,
    ) -> i32;
}

#[cfg(target_os = "windows")]
#[link(name = "kernel32")]
extern "system" {
    fn OpenFileMappingW(desired_access: u32, inherit_handle: i32, name: *const u16) -> isize;
    fn MapViewOfFile(mapping: isize, desired_access: u32, file_offset_high: u32, file_offset_low: u32, bytes: usize) -> *mut std::ffi::c_void;
    fn UnmapViewOfFile(address: *const std::ffi::c_void) -> i32;
    fn CloseHandle(handle: isize) -> i32;
}

pub fn run() {
    tauri::Builder::default()
        .manage(RuntimeState(Mutex::new(OwnedProcesses::default())))
        .invoke_handler(tauri::generate_handler![
            get_system_status,
            get_settings,
            get_bindings,
            save_bindings,
            get_controller_status,
            get_controller_command,
            save_settings,
            start_novr,
            stop_novr,
            set_view_mode,
            open_viewer,
            minimize_window,
            close_window
        ])
        .run(tauri::generate_context!())
        .expect("error while running Bigscreen NOVR Next");
}
