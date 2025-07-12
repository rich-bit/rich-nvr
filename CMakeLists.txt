cmake_minimum_required(VERSION 3.16)
project(RichMediaProject)

# --- Global target name used by subdirs ---
set(APP_TARGET richclient)

# --- Paths used by both top and subdirs ---
set(CORE_DIR  "${CMAKE_SOURCE_DIR}/core")
set(DIST_DIR  "${CMAKE_SOURCE_DIR}/dist")
set(ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")

# Build toggles
option(BUILD_CLIENT "Build the Qt client" ON)
option(BUILD_SERVER "Build the RTSP server (Live555/Linux-only)" ON)

# On Windows, force server OFF
if (WIN32)
  set(BUILD_SERVER OFF CACHE BOOL "Server disabled on Windows" FORCE)
endif()

# --- Qt6 (needed at top because we deploy on Windows) ---
find_package(Qt6 COMPONENTS Core Gui Widgets Network REQUIRED)

# --- nlohmann JSON (header-only library) ---
# We assume nlohmann-json3-dev is installed via apt
# No find_package needed for header-only library

# --- Create the executable ONCE (empty for now except core file) ---
add_executable(${APP_TARGET}
  ${CORE_DIR}/conversations.cpp)
target_include_directories(${APP_TARGET} PRIVATE ${CORE_DIR})
target_link_libraries(${APP_TARGET} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# --- Output to dist/ for all configs ---
if(CMAKE_CONFIGURATION_TYPES)
  foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
    set_target_properties(${APP_TARGET} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${DIST_DIR}"
    )
  endforeach()
else()
  set_target_properties(${APP_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DIST_DIR}"
  )
endif()

# --- Deploy Qt on Windows after build ---
if(WIN32)
  # If this path is odd on your machine, prefer using Qt6::windeployqt from Qt6DeploySupport
  add_custom_command(TARGET ${APP_TARGET} POST_BUILD
    COMMAND "${Qt6_DIR}/../../../bin/windeployqt.exe" "$<TARGET_FILE:${APP_TARGET}>"
    COMMENT "Deploying Qt runtime for ${APP_TARGET} to dist/"
  )
endif()

set(RICH_DIST_SERVER_DIR "${DIST_DIR}/server")

# --- Subprojects (these will add sources, include dirs, and link libs to APP_TARGET) ---
if (BUILD_SERVER)
  add_subdirectory(server)
else()
  message(STATUS "Skipping server build (BUILD_SERVER=OFF or Windows).")
endif()
# Subdirs
if (BUILD_CLIENT)
  add_subdirectory(client)
endif()

add_custom_command(TARGET ${APP_TARGET} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${ASSETS_DIR}/logo.png"
          "$<TARGET_FILE_DIR:${APP_TARGET}>/logo.png"
  COMMENT "Copying logo.png to dist/"
)
