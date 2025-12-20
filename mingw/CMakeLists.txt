cmake_minimum_required(VERSION 3.20)

project(nvrclient_mingw LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root = parent of this folder
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)

set(CLIENT_DIR "${PROJECT_ROOT}/client")
set(THIRD_PARTY_DIR "${PROJECT_ROOT}/third_party")
set(IMGUI_DIR "${THIRD_PARTY_DIR}/imgui")

# User-tunable roots (match your working command line defaults)
set(SDL2_ROOT "" CACHE PATH "SDL2 cross root (contains include/, lib/, bin/)")
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg root (contains include/, lib/, and typically bin/ for DLLs)")

# Defaults (override with -DSDL2_ROOT=... -DFFMPEG_ROOT=...)
if(SDL2_ROOT STREQUAL "")
  set(SDL2_ROOT "/home/rick/SDL2-2.28.5/x86_64-w64-mingw32" CACHE PATH "" FORCE)
endif()
if(FFMPEG_ROOT STREQUAL "")
  set(FFMPEG_ROOT "/home/rick/ffmpeg-8.0.1-full_build-shared" CACHE PATH "" FORCE)
endif()

add_executable(nvrclient
  "${CLIENT_DIR}/client.cpp"
  "${CLIENT_DIR}/ClientConfig.cpp"
  "${CLIENT_DIR}/ClientNetworking.cpp"
  "${CLIENT_DIR}/ConfigurationPanel.cpp"
  "${CLIENT_DIR}/AsyncNetworkWorker.cpp"

  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp"
)

set_target_properties(nvrclient PROPERTIES OUTPUT_NAME "nvrclient")

target_include_directories(nvrclient PRIVATE
  "${PROJECT_ROOT}"
  "${CLIENT_DIR}"
  "${THIRD_PARTY_DIR}"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"

  "${SDL2_ROOT}/include"
  "${SDL2_ROOT}/include/SDL2"
  "${FFMPEG_ROOT}/include"
)

# Match your working flags
if(MINGW)
  target_link_options(nvrclient PRIVATE -static-libgcc -static-libstdc++)
endif()

# Keep the optimization consistent with your manual build
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nvrclient PRIVATE $<$<CONFIG:Release>:-O2>)
endif()

# Library lookup
find_library(SDL2MAIN_LIB NAMES SDL2main HINTS "${SDL2_ROOT}/lib" REQUIRED)
find_library(SDL2_LIB     NAMES SDL2     HINTS "${SDL2_ROOT}/lib" REQUIRED)

find_library(AVFORMAT_LIB   NAMES avformat   HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVCODEC_LIB    NAMES avcodec    HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVUTIL_LIB     NAMES avutil     HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(SWSCALE_LIB    NAMES swscale    HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(SWRESAMPLE_LIB NAMES swresample HINTS "${FFMPEG_ROOT}/lib" REQUIRED)

# Ensure the linker can find import libs from the provided prefixes.
target_link_directories(nvrclient PRIVATE
  "${SDL2_ROOT}/lib"
  "${FFMPEG_ROOT}/lib"
)

# Windows system libs you had on the command line
target_link_libraries(nvrclient PRIVATE
  mingw32
  "${SDL2MAIN_LIB}"
  "${SDL2_LIB}"

  "${AVFORMAT_LIB}"
  "${AVCODEC_LIB}"
  "${AVUTIL_LIB}"
  "${SWSCALE_LIB}"
  "${SWRESAMPLE_LIB}"

  ws2_32
  mswsock
  secur32
  bcrypt
)

# -------- dist staging --------
# Default requested output: /dist/client
set(RICH_DIST_DIR "${PROJECT_ROOT}/dist/client" CACHE PATH "Where to stage nvrclient.exe + DLLs")

install(TARGETS nvrclient RUNTIME DESTINATION ".")

# Collect DLLs to copy
set(_dlls_to_install "")

# SDL2 DLL
foreach(_cand
  "${SDL2_ROOT}/bin/SDL2.dll"
  "${SDL2_ROOT}/SDL2.dll"
)
  if(EXISTS "${_cand}")
    list(APPEND _dlls_to_install "${_cand}")
    break()
  endif()
endforeach()

# FFmpeg DLLs: prefer bin/*.dll, else lib/*.dll
if(EXISTS "${FFMPEG_ROOT}/bin")
  file(GLOB _ffmpeg_dlls "${FFMPEG_ROOT}/bin/*.dll")
  list(APPEND _dlls_to_install ${_ffmpeg_dlls})
elseif(EXISTS "${FFMPEG_ROOT}/lib")
  file(GLOB _ffmpeg_dlls "${FFMPEG_ROOT}/lib/*.dll")
  list(APPEND _dlls_to_install ${_ffmpeg_dlls})
endif()

# MinGW runtime DLLs (best effort). With -static-libgcc/-static-libstdc++ these may be unnecessary.
function(_mingw_print_dll out_var dll_name)
  if(NOT CMAKE_CXX_COMPILER)
    return()
  endif()
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${dll_name}
    OUTPUT_VARIABLE _p
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  # If not found, MinGW prints the input name back.
  if(_p AND NOT _p STREQUAL "${dll_name}" AND EXISTS "${_p}")
    set(${out_var} "${_p}" PARENT_SCOPE)
  endif()
endfunction()

foreach(_dll_name
  "libwinpthread-1.dll"
  "libgcc_s_seh-1.dll"
  "libgcc_s_dw2-1.dll"
  "libstdc++-6.dll"
)
  _mingw_print_dll(_found "${_dll_name}")
  if(_found)
    list(APPEND _dlls_to_install "${_found}")
  endif()
endforeach()

list(REMOVE_DUPLICATES _dlls_to_install)

if(_dlls_to_install)
  install(FILES ${_dlls_to_install} DESTINATION ".")
endif()

# Convenience target: build + stage into dist folder via cmake --install
add_custom_target(stage
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${RICH_DIST_DIR}"
  DEPENDS nvrclient
  VERBATIM
)
