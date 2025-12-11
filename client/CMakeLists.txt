cmake_minimum_required(VERSION 3.16)
project(RichClient)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale libswresample)
pkg_check_modules(HTTPLIB REQUIRED cpp-httplib)

add_executable(richclient
    client.cpp
    ClientConfig.cpp
    ClientNetworking.cpp
    ConfigurationPanel.cpp
    AsyncNetworkWorker.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

target_include_directories(richclient PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${HTTPLIB_INCLUDE_DIRS}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}
)

target_compile_options(richclient PRIVATE
    ${SDL2_CFLAGS_OTHER}
    ${FFMPEG_CFLAGS_OTHER}
    ${HTTPLIB_CFLAGS_OTHER}
)

target_link_libraries(richclient PRIVATE
    ${SDL2_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    pthread
    ${HTTPLIB_LIBRARIES}
)

set_target_properties(richclient PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RICH_DIST_CLIENT_DIR}"
)
