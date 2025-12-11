cmake_minimum_required(VERSION 3.16)
project(RichMediaProject)

set(DIST_DIR  "${CMAKE_SOURCE_DIR}/dist")

option(BUILD_CLIENT "Build the SDL/FFmpeg client" ON)

option(BUILD_SERVER "Build the RTSP server (Live555/Linux-only)" ON)

if (WIN32)
  set(BUILD_SERVER OFF CACHE BOOL "Server disabled on Windows" FORCE)
endif()

set(RICH_DIST_SERVER_DIR "${DIST_DIR}/server")
set(RICH_DIST_CLIENT_DIR "${DIST_DIR}/client")

if (BUILD_SERVER)
  add_subdirectory(server)
else()
  message(STATUS "Skipping server build (BUILD_SERVER=OFF or Windows).")
endif()

if (BUILD_CLIENT)
  add_subdirectory(client)
else()
  message(STATUS "Skipping client build (BUILD_CLIENT=OFF)")
endif()
