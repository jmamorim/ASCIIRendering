cmake_minimum_required(VERSION 3.24)
project(ASCIIRendering C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# SDL3: window, renderer, file dialog, clipboard. Use the system copy if there
# is one (Linux: pacman -S sdl3 / apt install libsdl3-dev), otherwise build it.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# Dear ImGui: sliders, buttons, layout
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b
    GIT_SHALLOW TRUE)
# stb_image (image loading) and stb_truetype (glyph rasterizing)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(imgui stb)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# The glyph font is compiled into the program so it runs the same everywhere
set(FONT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/font_data.h)
add_custom_command(
    OUTPUT ${FONT_HEADER}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/assets/DejaVuSansMono.ttf
            -DOUT=${FONT_HEADER} -DNAME=font_data -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS assets/DejaVuSansMono.ttf cmake/embed.cmake)

add_executable(ASCIIRendering WIN32 ASCIIRendering/ASCIIRendering.cpp ${FONT_HEADER})
target_include_directories(ASCIIRendering PRIVATE ${stb_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(ASCIIRendering PRIVATE imgui SDL3::SDL3)
if(MSVC)
    target_compile_options(ASCIIRendering PRIVATE /utf-8)
endif()

# Web build (emcmake cmake ...): index.html + index.js + index.wasm
if(EMSCRIPTEN)
    set_target_properties(ASCIIRendering PROPERTIES OUTPUT_NAME index SUFFIX .html)
    target_link_options(ASCIIRendering PRIVATE
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=4GB
        -sMINIFY_HTML=0
        --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
    set_property(TARGET ASCIIRendering APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
endif()
