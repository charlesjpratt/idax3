cmake_minimum_required(VERSION 3.20)
# The version here is the one the title screen shows, so a release is cut by
# bumping it and tagging the same number.
project(Bouncer_CPP VERSION 1.0.1 LANGUAGES CXX)

# Vendored deps still declare old cmake_minimum_required values — required when
# building with CMake 4.x.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The C runtime is linked in rather than looked for, so a release runs on a
# machine with no redistributable installed. Set before the dependencies so
# SDL2 is built against the same one.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ── Dependencies ─────────────────────────────────────────────────────────────
include(FetchContent)

# SDL2 is linked in, not shipped beside the exe: the game is one file. Static
# SDL wants the system C library rather than its own stand-ins, or the two
# fight over memcpy and friends at link time.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_LIBC   ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.30.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL2)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# stb_truetype rasterizes the title screen's font. One public-domain header,
# pulled on its own rather than with the whole stb repository behind it, and
# pinned by commit and checksum so it is the same file every time.
FetchContent_Declare(
    stb_truetype
    URL https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h
    URL_HASH SHA256=ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb_truetype)

# ── The font, built in ───────────────────────────────────────────────────────
# The title's face is turned into a header of bytes at build time, so the exe
# carries it and there is no assets folder to keep beside it. Regenerated when
# the .ttf changes; `title.font` in config.json can still point at another.
set(FONT_FILE   ${CMAKE_SOURCE_DIR}/assets/Bungee-Regular.ttf)
set(FONT_HEADER ${CMAKE_BINARY_DIR}/generated/font_data.hpp)
add_custom_command(
    OUTPUT  ${FONT_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${FONT_FILE} -DOUTPUT=${FONT_HEADER} -DNAME=kFontData
        -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${FONT_FILE} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding ${FONT_FILE}"
)

# ── Game executable ──────────────────────────────────────────────────────────
add_executable(Bouncer_CPP src/main.cpp ${FONT_HEADER})

# The file is the game's name, whatever the target is called. Windows gets a
# GUI-subsystem exe, so launching it does not also open a console: SDL2main
# supplies the WinMain that hands over to main().
set_target_properties(Bouncer_CPP PROPERTIES
    OUTPUT_NAME      IDAIDAIDA
    WIN32_EXECUTABLE $<BOOL:${WIN32}>
)

target_include_directories(Bouncer_CPP PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${stb_truetype_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(Bouncer_CPP PRIVATE
    GAME_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(Bouncer_CPP PRIVATE
    SDL2::SDL2-static
    SDL2::SDL2main
    nlohmann_json::nlohmann_json
)

target_compile_options(Bouncer_CPP PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
