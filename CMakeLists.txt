cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(PkgConfig REQUIRED)

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

pkg_check_modules(
  GSTREAMER
  REQUIRED
  IMPORTED_TARGET
  gstreamer-1.0>=1.24
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-rtsp-1.0
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs PkgConfig::GSTREAMER)

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp)
endif()

# On Windows, OBS loads plugins with a restricted DLL search path. Delay-load
# the GStreamer runtime so the module itself can load first; obs_module_load()
# then points Windows at the official GStreamer runtime before gst_init_check().
if(MSVC)
  target_link_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      "/DELAYLOAD:gstapp-1.0-0.dll"
      "/DELAYLOAD:gstvideo-1.0-0.dll"
      "/DELAYLOAD:gstreamer-1.0-0.dll"
      "/DELAYLOAD:gobject-2.0-0.dll"
      "/DELAYLOAD:glib-2.0-0.dll"
  )
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE delayimp)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/low-latency-rtsp.c
    src/ui-status.cpp
    src/ui-status.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
