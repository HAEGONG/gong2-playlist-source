cmake_minimum_required(VERSION 3.28...3.30)

# Two build modes:
#
#   default (CI / releases): the obs-plugintemplate build. Downloads the pinned OBS sources and
#     dependencies from buildspec.json and builds with the platform presets in CMakePresets.json
#     (Xcode universal on macOS, Visual Studio on Windows, Ninja on Linux).
#   GONG2_LOCAL_OBS_APP=ON (macOS only): a quick Xcode-free build against the OBS.app that is
#     installed on this machine. Used by scripts/build-macos.sh.
option(GONG2_LOCAL_OBS_APP "macOS: build against the installed OBS.app instead of the plugin template deps" OFF)

set(
  GONG2_SOURCES
  src/plugin-main.cpp
  src/playlist-source.hpp
  src/playlist-source.cpp
  src/resume-store.hpp
  src/resume-store.cpp
  src/playlist-window.hpp
  src/playlist-window.cpp
)

if(GONG2_LOCAL_OBS_APP)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _buildspec)
  string(JSON _name GET ${_buildspec} name)
  string(JSON _version GET ${_buildspec} version)
  string(JSON GONG2_MACOS_BUNDLE_ID GET ${_buildspec} platformConfig macos bundleId)

  project(${_name} VERSION ${_version} LANGUAGES C CXX)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(PLUGIN_NAME ${_name})

  include(cmake/local-obs-app.cmake)
  return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
            $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  # AUTOMOC_COMPILER_PREDEFINES OFF: with Xcode 26, running the compiler from the AUTOMOC script
  # phase fails with "conflicting deployment targets ... present in environment".
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON AUTOMOC_COMPILER_PREDEFINES OFF
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${GONG2_SOURCES})

# The sources print the version with this macro (the template's plugin-support.c is not used).
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PLUGIN_VERSION="${_version}")

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
