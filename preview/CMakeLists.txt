cmake_minimum_required(VERSION 3.20)
project(BigscreenNOVRPreview LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Bigscreen NOVR Preview currently targets Windows only.")
endif()

add_executable(BigscreenNOVR WIN32
  "${CMAKE_CURRENT_LIST_DIR}/../src/bigscreen_novr_main.cpp"
)
target_compile_features(BigscreenNOVR PRIVATE cxx_std_17)
target_compile_definitions(BigscreenNOVR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _AMD64_ _WIN64 UNICODE _UNICODE)
target_link_libraries(BigscreenNOVR PRIVATE shell32 dwmapi)
set_target_properties(BigscreenNOVR PROPERTIES
  OUTPUT_NAME "BigscreenNOVR"
  WIN32_EXECUTABLE TRUE
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
