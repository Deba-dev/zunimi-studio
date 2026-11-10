cmake_minimum_required(VERSION 3.28)
project(ZunimiStudio VERSION 0.2.0 LANGUAGES C CXX)

# Zunimi Studio links against a libobs built from the OBS source with the "zunimi" preset
# (see scripts/build-obs.ps1). Everything lives on D: by default; override these on the command line.
set(OBS_SOURCE_DIR "D:/zunimi-build/obs-studio" CACHE PATH "OBS Studio source tree")
set(OBS_BUILD_DIR "${OBS_SOURCE_DIR}/build_zunimi" CACHE PATH "OBS build tree made with the zunimi preset")
set(OBS_CONFIG "RelWithDebInfo" CACHE STRING "Configuration libobs was built with")

file(GLOB QT_DIR LIST_DIRECTORIES true "${OBS_SOURCE_DIR}/.deps/obs-deps-qt6-*-x64")
list(APPEND CMAKE_PREFIX_PATH "${QT_DIR}")
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Svg)

add_library(libobs SHARED IMPORTED)
set_target_properties(libobs PROPERTIES
  IMPORTED_IMPLIB "${OBS_BUILD_DIR}/libobs/${OBS_CONFIG}/obs.lib"
  IMPORTED_LOCATION "${OBS_BUILD_DIR}/libobs/${OBS_CONFIG}/obs.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${OBS_BUILD_DIR}/config;${OBS_SOURCE_DIR}/deps/w32-pthreads")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

add_executable(zunimi-studio WIN32
  src/main.cpp
  src/core/Settings.cpp src/core/Settings.h
  src/core/Hardware.cpp src/core/Hardware.h
  src/core/ObsCore.cpp src/core/ObsCore.h
  src/core/Updater.cpp src/core/Updater.h
  src/ui/Theme.cpp src/ui/Theme.h
  src/ui/PreviewWidget.cpp src/ui/PreviewWidget.h
  src/ui/WindowModes.cpp src/ui/WindowModes.h
  src/ui/SettingsDialog.cpp src/ui/SettingsDialog.h
  src/ui/MainWindow.cpp src/ui/MainWindow.h
  src/ui/PropertiesView.cpp src/ui/PropertiesView.h
  src/ui/FiltersDialog.cpp src/ui/FiltersDialog.h
  src/ui/Mixer.cpp src/ui/Mixer.h
  src/ui/Hotkeys.cpp src/ui/Hotkeys.h
  resources/zunimi.qrc
  resources/app.rc)

target_include_directories(zunimi-studio PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(zunimi-studio PRIVATE Qt6::Widgets Qt6::Network libobs crypt32 dxgi)
target_compile_definitions(zunimi-studio PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(zunimi-studio PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus)

# Turns resources/logo.svg into resources/zunimi.ico and the logo PNGs (scripts/build.ps1 -Icon). Not shipped.
add_executable(make-icon EXCLUDE_FROM_ALL tools/make-icon.cpp)
target_link_libraries(make-icon PRIVATE Qt6::Gui Qt6::Svg)
