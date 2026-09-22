cmake_minimum_required(VERSION 3.20)
project(Bouncer_CPP VERSION 0.1.0 LANGUAGES CXX)

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

# ── Game executable ──────────────────────────────────────────────────────────
add_executable(Bouncer_CPP src/main.cpp)

target_include_directories(Bouncer_CPP PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${stb_truetype_SOURCE_DIR}
)

target_link_libraries(Bouncer_CPP PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    nlohmann_json::nlohmann_json
)

target_compile_options(Bouncer_CPP PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(WIN32)
    add_custom_command(TARGET Bouncer_CPP POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:Bouncer_CPP>
    )
endif()
