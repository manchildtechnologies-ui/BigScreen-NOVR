cmake_minimum_required(VERSION 3.20)
project(BigscreenDesktopBridge LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This prototype currently targets Windows only.")
endif()

set(OPENVR_SDK_ROOT "" CACHE PATH "Path to a Valve OpenVR SDK checkout")
if(NOT EXISTS "${OPENVR_SDK_ROOT}/headers/openvr_driver.h")
  message(FATAL_ERROR "Set OPENVR_SDK_ROOT to an OpenVR SDK checkout containing headers/openvr_driver.h")
endif()

add_library(driver_bigscreen_desktop SHARED
  src/driver.cpp
  src/direct_mode.cpp
)
target_include_directories(driver_bigscreen_desktop PRIVATE "${OPENVR_SDK_ROOT}/headers")
target_compile_features(driver_bigscreen_desktop PRIVATE cxx_std_17)
target_compile_definitions(driver_bigscreen_desktop PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_link_libraries(driver_bigscreen_desktop PRIVATE d3d11 dxgi)
set_target_properties(driver_bigscreen_desktop PROPERTIES
  OUTPUT_NAME "driver_bigscreen_desktop"
  PREFIX ""
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

set(DRIVER_ROOT "${CMAKE_CURRENT_BINARY_DIR}/BigscreenDesktopDriver")
add_custom_command(TARGET driver_bigscreen_desktop POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVER_ROOT}/bin/win64"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVER_ROOT}/resources/settings"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:driver_bigscreen_desktop>" "${DRIVER_ROOT}/bin/win64/driver_bigscreen_desktop.dll"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest" "${DRIVER_ROOT}/driver.vrdrivermanifest"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/resources/settings/default.vrsettings" "${DRIVER_ROOT}/resources/settings/default.vrsettings"
)

add_executable(BigscreenDesktopBridge WIN32
  src/bridge_main.cpp
)
target_include_directories(BigscreenDesktopBridge PRIVATE src)
target_compile_features(BigscreenDesktopBridge PRIVATE cxx_std_17)
target_compile_definitions(BigscreenDesktopBridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
set_target_properties(BigscreenDesktopBridge PROPERTIES
  OUTPUT_NAME "BigscreenDesktopBridge"
  WIN32_EXECUTABLE FALSE
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_executable(BigscreenDesktopViewer WIN32
  src/viewer_main.cpp
)
target_include_directories(BigscreenDesktopViewer PRIVATE "${OPENVR_SDK_ROOT}/headers")
target_compile_features(BigscreenDesktopViewer PRIVATE cxx_std_17)
target_compile_definitions(BigscreenDesktopViewer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_include_directories(BigscreenDesktopViewer PRIVATE src)
target_link_libraries(BigscreenDesktopViewer PRIVATE
  "${OPENVR_SDK_ROOT}/lib/win64/openvr_api.lib"
  d3d11 dxgi d3dcompiler
)
set_target_properties(BigscreenDesktopViewer PROPERTIES
  OUTPUT_NAME "BigscreenDesktopViewer"
  WIN32_EXECUTABLE FALSE
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
add_custom_command(TARGET BigscreenDesktopViewer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENVR_SDK_ROOT}/bin/win64/openvr_api.dll"
    "$<TARGET_FILE_DIR:BigscreenDesktopViewer>/openvr_api.dll"
)

add_executable(BigscreenFrameDump
  src/frame_dump.cpp
)
target_compile_features(BigscreenFrameDump PRIVATE cxx_std_17)
target_compile_definitions(BigscreenFrameDump PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64)
target_link_libraries(BigscreenFrameDump PRIVATE windowscodecs)
set_target_properties(BigscreenFrameDump PROPERTIES
  OUTPUT_NAME "BigscreenFrameDump"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_executable(BigscreenAppDiag
  phase8-state/app_diag.cpp
)
target_include_directories(BigscreenAppDiag PRIVATE "${OPENVR_SDK_ROOT}/headers")
target_compile_features(BigscreenAppDiag PRIVATE cxx_std_17)
target_link_libraries(BigscreenAppDiag PRIVATE "${OPENVR_SDK_ROOT}/lib/win64/openvr_api.lib")
set_target_properties(BigscreenAppDiag PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
