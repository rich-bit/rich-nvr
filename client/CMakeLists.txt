cmake_minimum_required(VERSION 3.16)
project(RichNVR)

if(NOT DEFINED APP_TARGET OR NOT TARGET ${APP_TARGET})
  message(FATAL_ERROR "APP_TARGET must be created in the top-level before add_subdirectory(client)")
endif()

set(CMAKE_CXX_STANDARD 17)

set(GUI_DIR    "${CMAKE_CURRENT_SOURCE_DIR}")
set(CORE_DIR   "${CMAKE_SOURCE_DIR}/core")

message(STATUS "Using Qt from: ${CMAKE_PREFIX_PATH}")
message(STATUS "Source dir: ${CMAKE_SOURCE_DIR}")
message(STATUS "App target: ${APP_TARGET}")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# ---------------- GStreamer/GLib discovery ----------------
if (WIN32)
  if (NOT GStreamer_DEV_DIR)
    message(FATAL_ERROR "Set -DGStreamer_DEV_DIR=... (e.g. C:/gstreamer/1.0/msvc_x86_64)")
  endif()

  # Include dirs (note the two .../lib/.../include dirs for config headers)
  set(GST_WIN_INCLUDE_DIRS
    "${GStreamer_DEV_DIR}/include/gstreamer-1.0"
    "${GStreamer_DEV_DIR}/include/glib-2.0"
    "${GStreamer_DEV_DIR}/lib/gstreamer-1.0/include"
    "${GStreamer_DEV_DIR}/lib/glib-2.0/include"
    "${GStreamer_DEV_DIR}/include"
  )
  set(GST_WIN_LIB_DIR "${GStreamer_DEV_DIR}/lib")
  # Link libs you actually use
  set(GST_WIN_LIBS
    gstreamer-1.0.lib
    gstapp-1.0.lib
    gstvideo-1.0.lib
    gstpbutils-1.0.lib
    gstaudio-1.0.lib
    gstbase-1.0.lib
    gobject-2.0.lib
    glib-2.0.lib
    gmodule-2.0.lib
  )
  # Some GLib bundles provide libintl separately; add if you have it
  if (EXISTS "${GST_WIN_LIB_DIR}/intl.lib")
    list(APPEND GST_WIN_LIBS intl.lib)
  endif()

else() # UNIX (Linux) — use pkg-config
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GST REQUIRED
    gstreamer-1.0>=1.18
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-pbutils-1.0
  )
  pkg_check_modules(GLIB REQUIRED glib-2.0>=2.56 gobject-2.0)

  message(STATUS "GST libs:   ${GST_LIBRARIES}")
  message(STATUS "GLib libs:  ${GLIB_LIBRARIES}")
  message(STATUS "GST cflags: ${GST_CFLAGS_OTHER}")
  message(STATUS "GLib cflags:${GLIB_CFLAGS_OTHER}")
endif()

# ---------------- Sources ----------------
set(CLIENT_SOURCES
  ${GUI_DIR}/main.cpp
  ${GUI_DIR}/VideoPlayer.cpp
  ${GUI_DIR}/FileBrowserWindow.cpp
  ${GUI_DIR}/Dashboard.cpp
  ${GUI_DIR}/AddStreamDialog.cpp
  ${GUI_DIR}/OverlayBox.cpp
  ${GUI_DIR}/CamerasInfoDialog.cpp
  ${GUI_DIR}/MotionFrameDialog.cpp
  ${GUI_DIR}/Remote.cpp
  ${GUI_DIR}/RemoteServerDialog.cpp
  ${GUI_DIR}/VideoWrapper.cpp
  ${GUI_DIR}/FloatingOverlay.cpp
  ${GUI_DIR}/VideoSurface.cpp
  ../core/Settings.cpp
  ../core/PathUtils.cpp
)
set(CLIENT_HEADERS
  ${GUI_DIR}/VideoPlayer.h
  ${GUI_DIR}/FileBrowserWindow.h
  ${GUI_DIR}/Dashboard.h
  ${GUI_DIR}/AddStreamDialog.h
  ${GUI_DIR}/OverlayBox.h
  ${GUI_DIR}/CamerasInfoDialog.h
  ${GUI_DIR}/MotionFrameDialog.h
  ${GUI_DIR}/Remote.h
  ${GUI_DIR}/RemoteServerDialog.h
  ${GUI_DIR}/VideoWrapper.h
  ${GUI_DIR}/FloatingOverlay.h
)
target_sources(${APP_TARGET} PRIVATE ${CLIENT_SOURCES} ${CLIENT_HEADERS})

# ---------------- Include paths ----------------
target_include_directories(${APP_TARGET} PRIVATE
  ${CORE_DIR}
)

if (WIN32)
  target_include_directories(${APP_TARGET} PRIVATE ${GST_WIN_INCLUDE_DIRS})
  target_link_directories(${APP_TARGET} PRIVATE ${GST_WIN_LIB_DIR})
else()
  target_include_directories(${APP_TARGET} PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
  )
  if(GST_LIBRARY_DIRS)
    target_link_directories(${APP_TARGET} PRIVATE ${GST_LIBRARY_DIRS})
  endif()
  if(GLIB_LIBRARY_DIRS)
    target_link_directories(${APP_TARGET} PRIVATE ${GLIB_LIBRARY_DIRS})
  endif()
  if(GST_CFLAGS_OTHER)
    target_compile_options(${APP_TARGET} PRIVATE ${GST_CFLAGS_OTHER})
  endif()
  if(GLIB_CFLAGS_OTHER)
    target_compile_options(${APP_TARGET} PRIVATE ${GLIB_CFLAGS_OTHER})
  endif()
endif()

# ---------------- Link ----------------
target_link_libraries(${APP_TARGET} PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
)

if (WIN32)
  target_link_libraries(${APP_TARGET} PRIVATE ${GST_WIN_LIBS})

  # Optional OpenCV for the client (only if provided)
  if (DEFINED OpenCV_DIR)
    find_package(OpenCV REQUIRED)
    target_link_libraries(${APP_TARGET} PRIVATE ${OpenCV_LIBS})
    target_include_directories(${APP_TARGET} PRIVATE ${OpenCV_INCLUDE_DIRS})
  endif()

  # Make debugging from VS easy: add GStreamer bin & plugin dir to PATH
  if (DEFINED GStreamer_RUNTIME_DIR)
    file(TO_CMAKE_PATH "${GStreamer_RUNTIME_DIR}" _GST_RT)
    set(_GST_DBG_ENV "PATH=%PATH%;${_GST_RT}/bin;${_GST_RT}/lib/gstreamer-1.0")
    set_target_properties(${APP_TARGET} PROPERTIES VS_DEBUGGER_ENVIRONMENT "${_GST_DBG_ENV}")
  endif()
else()
  target_link_libraries(${APP_TARGET} PRIVATE
    ${GST_LIBRARIES}
    ${GLIB_LIBRARIES}
    X11  # Add X11 library for WSL X11 error handling
  )
  target_link_options(${APP_TARGET} PRIVATE -Wl,--no-as-needed)
endif()

# ===== Windows runtime deployment (copy DLLs/plugins next to the exe) =====
if (WIN32)
  # Where to copy files: the folder that contains the built exe for the active config
  set(_APP_OUT "$<TARGET_FILE_DIR:${APP_TARGET}>")

  # Normalize GStreamer runtime path if provided
  set(GST_RT "")
  if (DEFINED GStreamer_RUNTIME_DIR AND EXISTS "${GStreamer_RUNTIME_DIR}")
    file(TO_CMAKE_PATH "${GStreamer_RUNTIME_DIR}" GST_RT)
  else()
    message(WARNING "GStreamer_RUNTIME_DIR not set or invalid; skipping GStreamer runtime copy.")
  endif()

  # Build the command list
  set(_DEPLOY_CMDS
    COMMAND ${CMAKE_COMMAND} -E echo "Deploying runtime to ${_APP_OUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_APP_OUT}"
  )

  # ---- GStreamer (DLLs + plugins + helpers) ----
  if (GST_RT)
    list(APPEND _DEPLOY_CMDS
      # plugins
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_APP_OUT}/lib/gstreamer-1.0"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${GST_RT}/lib/gstreamer-1.0" "${_APP_OUT}/lib/gstreamer-1.0"
      # DLLs (bin)
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${GST_RT}/bin" "${_APP_OUT}"
    )
    if (EXISTS "${GST_RT}/libexec/gstreamer-1.0/gst-plugin-scanner.exe")
      list(APPEND _DEPLOY_CMDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_APP_OUT}/libexec/gstreamer-1.0"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GST_RT}/libexec/gstreamer-1.0/gst-plugin-scanner.exe"
                "${_APP_OUT}/libexec/gstreamer-1.0"
      )
    endif()
    if (EXISTS "${GST_RT}/bin/gspawn-win64-helper.exe")
      list(APPEND _DEPLOY_CMDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GST_RT}/bin/gspawn-win64-helper.exe" "${_APP_OUT}"
      )
    endif()
    if (EXISTS "${GST_RT}/bin/gspawn-win64-helper-console.exe")
      list(APPEND _DEPLOY_CMDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GST_RT}/bin/gspawn-win64-helper-console.exe" "${_APP_OUT}"
      )
    endif()
  endif()

  # ---- Qt (core DLLs + plugins) ----
  set(QT_BIN_DIR      "${CMAKE_PREFIX_PATH}/bin")
  set(QT_PLUGINS_DIR  "${CMAKE_PREFIX_PATH}/plugins")

  foreach(dll IN ITEMS Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll Qt6Network.dll)
    list(APPEND _DEPLOY_CMDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${QT_BIN_DIR}/${dll}" "${_APP_OUT}"
    )
  endforeach()

  list(APPEND _DEPLOY_CMDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_APP_OUT}/plugins/platforms"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_APP_OUT}/plugins/imageformats"
  )
  if (EXISTS "${QT_PLUGINS_DIR}/platforms/qwindows.dll")
    list(APPEND _DEPLOY_CMDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${QT_PLUGINS_DIR}/platforms/qwindows.dll"
              "${_APP_OUT}/plugins/platforms/"
    )
  endif()
  if (EXISTS "${QT_PLUGINS_DIR}/imageformats/qjpeg.dll")
    list(APPEND _DEPLOY_CMDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${QT_PLUGINS_DIR}/imageformats/qjpeg.dll"
              "${_APP_OUT}/plugins/imageformats/"
    )
  endif()

  # Make a separate deploy target that runs after the exe is built
  add_custom_target(${APP_TARGET}_deploy ALL
    DEPENDS ${APP_TARGET}
    ${_DEPLOY_CMDS}
  )
endif()

# ---------------- Qt autogen ----------------
set_target_properties(${APP_TARGET} PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
  AUTORCC ON
)
if(COMMAND qt_finalize_executable)
  qt_finalize_executable(${APP_TARGET})
endif()
