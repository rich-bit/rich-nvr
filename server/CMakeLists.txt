cmake_minimum_required(VERSION 3.16)
project(NVRServer)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenCV REQUIRED COMPONENTS core imgproc video imgcodecs)
# Qt6 still needed for VideoExporter and conversations (TODO: replace with std:: alternatives)
find_package(Qt6 REQUIRED COMPONENTS Core)

if(DEFINED LIVE555_DIR)
  # Use custom LIVE555 build
  message(STATUS "Using custom LIVE555 from: ${LIVE555_DIR}")
  
  set(LIVE555_LIVEMEDIA_INCLUDE ${LIVE555_DIR}/liveMedia/include)
  set(LIVE555_GROUPSOCK_INCLUDE ${LIVE555_DIR}/groupsock/include)
  set(LIVE555_USAGEENV_INCLUDE ${LIVE555_DIR}/UsageEnvironment/include)
  set(LIVE555_BASICENV_INCLUDE ${LIVE555_DIR}/BasicUsageEnvironment/include)
  
  set(LIVE555_LIVEMEDIA_LIB ${LIVE555_DIR}/liveMedia/libliveMedia.a)
  set(LIVE555_GROUPSOCK_LIB ${LIVE555_DIR}/groupsock/libgroupsock.a)
  set(LIVE555_USAGEENV_LIB ${LIVE555_DIR}/UsageEnvironment/libUsageEnvironment.a)
  set(LIVE555_BASICUSAGEENV_LIB ${LIVE555_DIR}/BasicUsageEnvironment/libBasicUsageEnvironment.a)
  
  # Verify custom build files exist
  if(NOT EXISTS ${LIVE555_LIVEMEDIA_LIB})
    message(FATAL_ERROR "Custom LIVE555 library not found: ${LIVE555_LIVEMEDIA_LIB}\nBuild LIVE555 first with 'make' in ${LIVE555_DIR}")
  endif()
  
  message(STATUS "  liveMedia: ${LIVE555_LIVEMEDIA_LIB}")
  message(STATUS "  groupsock: ${LIVE555_GROUPSOCK_LIB}")
  message(STATUS "  UsageEnvironment: ${LIVE555_USAGEENV_LIB}")
  message(STATUS "  BasicUsageEnvironment: ${LIVE555_BASICUSAGEENV_LIB}")
else()
  # Use system LIVE555
  message(STATUS "Using system LIVE555")
  
  list(APPEND LIVE555_HINTS
    /usr/local /usr/local/include /usr/local/lib
    /usr /usr/include /usr/lib)

  # Headers
  find_path(LIVE555_LIVEMEDIA_INCLUDE  liveMedia.hh
    HINTS ${LIVE555_HINTS}
    PATH_SUFFIXES liveMedia)
  find_path(LIVE555_GROUPSOCK_INCLUDE  GroupsockHelper.hh
    HINTS ${LIVE555_HINTS}
    PATH_SUFFIXES groupsock)
  find_path(LIVE555_USAGEENV_INCLUDE   UsageEnvironment.hh
    HINTS ${LIVE555_HINTS}
    PATH_SUFFIXES UsageEnvironment)
  find_path(LIVE555_BASICENV_INCLUDE   BasicUsageEnvironment.hh
    HINTS ${LIVE555_HINTS}
    PATH_SUFFIXES BasicUsageEnvironment)

  # Libs
  find_library(LIVE555_LIVEMEDIA_LIB         liveMedia        HINTS ${LIVE555_HINTS})
  find_library(LIVE555_GROUPSOCK_LIB         groupsock        HINTS ${LIVE555_HINTS})
  find_library(LIVE555_USAGEENV_LIB          UsageEnvironment HINTS ${LIVE555_HINTS})
  find_library(LIVE555_BASICUSAGEENV_LIB     BasicUsageEnvironment HINTS ${LIVE555_HINTS})

  # Fail clearly if not found
  if(NOT LIVE555_LIVEMEDIA_INCLUDE OR NOT LIVE555_BASICENV_INCLUDE OR
     NOT LIVE555_GROUPSOCK_INCLUDE OR NOT LIVE555_USAGEENV_INCLUDE)
    message(FATAL_ERROR "Live555 headers not found. Install 'liblivemedia-dev' or pass -DLIVE555_DIR=/path/to/live.")
  endif()
  if(NOT LIVE555_LIVEMEDIA_LIB OR NOT LIVE555_GROUPSOCK_LIB OR
     NOT LIVE555_USAGEENV_LIB OR NOT LIVE555_BASICUSAGEENV_LIB)
    message(FATAL_ERROR "Live555 libraries not found. Install 'liblivemedia-dev' or pass -DLIVE555_DIR=/path/to/live.")
  endif()
endif()

include_directories(${CMAKE_SOURCE_DIR}/core
                    ${OpenCV_INCLUDE_DIRS})

# Linux: Use pkg-config to find GStreamer and httplib
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.18 gstreamer-base-1.0 gstreamer-rtsp-server-1.0)
    pkg_check_modules(HTTPLIB REQUIRED cpp-httplib)
    include_directories(${GST_INCLUDE_DIRS} ${HTTPLIB_INCLUDE_DIRS})
    link_directories(${GST_LIBRARY_DIRS} ${HTTPLIB_LIBRARY_DIRS})
    set(GSTREAMER_LIBS ${GST_LIBRARIES})
else() # Windows
    if(GStreamer_DEV_DIR)
        set(GSTREAMER_INCLUDE_DIRS 
            "${GStreamer_DEV_DIR}/include/gstreamer-1.0" 
            "${GStreamer_DEV_DIR}/include/glib-2.0"
            "${GStreamer_DEV_DIR}/lib/gstreamer-1.0/include"
            "${GStreamer_DEV_DIR}/lib/glib-2.0/include"
            "${GStreamer_DEV_DIR}/include"
        )
        set(GSTREAMER_LIB_DIRS "${GStreamer_DEV_DIR}/lib")
        set(GSTREAMER_LIBS
            "${GStreamer_DEV_DIR}/lib/gstreamer-1.0.lib"
            "${GStreamer_DEV_DIR}/lib/gobject-2.0.lib"        
            "${GStreamer_DEV_DIR}/lib/glib-2.0.lib"
            "${GStreamer_DEV_DIR}/lib/gstvideo-1.0.lib"
            "${GStreamer_DEV_DIR}/lib/gstbase-1.0.lib"
            "${GStreamer_DEV_DIR}/lib/gmodule-2.0.lib"
            "gstrtspserver-1.0"
        )
    else()
        message(FATAL_ERROR "GStreamer_DEV_DIR is not set! Pass -DGStreamer_DEV_DIR=<path>")
    endif()
    include_directories(${GSTREAMER_INCLUDE_DIRS})
    link_directories(${GSTREAMER_LIB_DIRS})
endif()

add_library(NVRServerLib 
    ../core/CameraManager.cpp 
    ../core/CameraStream.cpp 
    ../core/PathUtils.cpp 
    ../core/Settings.cpp 
    ../core/SegmentWorker.cpp 
    ../core/VideoExporter.cpp
    ../core/conversations.cpp
    ../core/live555RtspProxy.cpp
    ../core/gstreamerRtspProxy.cpp)
    
add_executable(nvrserver main.cpp)

# Set output directory to root project's /dist/server for all configs
set(DIST_SERVER_DIR "${RICH_DIST_SERVER_DIR}")

# Set output directory
if(CMAKE_CONFIGURATION_TYPES)
    foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
        set_target_properties(nvrserver PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${DIST_SERVER_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${DIST_SERVER_DIR}"  # for .lib files
            LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${DIST_SERVER_DIR}"  # for .dll (if you ever build DLLs)
        )
    endforeach()
else()
    set_target_properties(nvrserver PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DIST_SERVER_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${DIST_SERVER_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${DIST_SERVER_DIR}"
    )
endif()

target_link_libraries(nvrserver NVRServerLib ${GSTREAMER_LIBS} ${HTTPLIB_LIBRARIES} ${OpenCV_LIBS} gstrtspserver-1.0 gstapp-1.0 Qt6::Core)

# Attach include dirs to the target that compiles live555RtspProxy.*
target_include_directories(NVRServerLib PUBLIC
  ${LIVE555_LIVEMEDIA_INCLUDE}
  ${LIVE555_GROUPSOCK_INCLUDE}
  ${LIVE555_USAGEENV_INCLUDE}
  ${LIVE555_BASICENV_INCLUDE}
)

# Link the libs (you already do, this just ensures order & grouping)
target_link_libraries(NVRServerLib
  ${LIVE555_LIVEMEDIA_LIB}
  ${LIVE555_GROUPSOCK_LIB}
  ${LIVE555_BASICUSAGEENV_LIB}
  ${LIVE555_USAGEENV_LIB}
  Qt6::Core
  ssl crypto
)

if(WIN32 AND GStreamer_RUNTIME_DIR)
    file(GLOB GST_DLLS "${GStreamer_RUNTIME_DIR}/bin/*.dll")
    foreach(gst_dll ${GST_DLLS})
        add_custom_command(TARGET nvrserver POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${gst_dll}"
                "${DIST_SERVER_DIR}/"
        )
    endforeach()

    # 2. Copy the whole plugins directory
    add_custom_command(TARGET nvrserver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RICH_DIST_SERVER_DIR}/lib/gstreamer-1.0"
    )
    add_custom_command(TARGET nvrserver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GStreamer_RUNTIME_DIR}/lib/gstreamer-1.0"
            "${RICH_DIST_SERVER_DIR}/lib/gstreamer-1.0"
    )

endif()
