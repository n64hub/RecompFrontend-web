cmake_minimum_required(VERSION 3.20)

project(recompinput)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB CXX_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Define the library
add_library(recompinput STATIC
    ${CXX_SOURCES}
)

SET(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/lunasvg)
SET(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
SET(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
SET(LUNASVG_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)

target_include_directories(recompinput PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/GamepadMotionHelpers
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompui/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../recompui/lib/RmlUi/Include
    ${RECOMP_FRONTEND_N64MODERNRUNTIME_PATH}/librecomp/include
    ${RECOMP_FRONTEND_N64MODERNRUNTIME_PATH}/N64Recomp/include
    ${RECOMP_FRONTEND_N64MODERNRUNTIME_PATH}/ultramodern/include
    ${RECOMP_FRONTEND_N64MODERNRUNTIME_PATH}/thirdparty
    ${RECOMP_FRONTEND_N64MODERNRUNTIME_PATH}/thirdparty/concurrentqueue
)

target_include_directories(recompinput PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/recompinput"
)

if (WIN32)
    target_include_directories(recompinput PRIVATE ${sdl2_SOURCE_DIR}/include)
elseif (APPLE OR CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_include_directories(recompinput PRIVATE ${SDL2_INCLUDE_DIRS})
elseif (EMSCRIPTEN)
    # Emscripten's port of SDL2.
    target_compile_options(recompinput PRIVATE -sUSE_SDL=2)
    target_include_directories(recompinput PRIVATE ${EMSCRIPTEN_SYSROOT}/include/SDL2)
endif()

target_compile_options(recompinput PRIVATE
    -Wno-unused-parameter
)

if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

target_link_libraries(recompinput PUBLIC)

# A game that turns single-player mode off (set_single_player_mode(false)) plays as single-player mode does, every
# controller and the keyboard as player 1, until players are assigned (src/players.cpp, is_single_player_mode), and an
# assigned player then reads only their own device (src/profiles.cpp, get_n64_input).
option(RECOMPINPUT_SINGLE_PLAYER_UNTIL_ASSIGNED "A multiplayer game plays as single-player mode until players are assigned; then the keyboard plays only its player, or player 1 while nobody has it (always on with Emscripten)" OFF)
if (RECOMPINPUT_SINGLE_PLAYER_UNTIL_ASSIGNED OR EMSCRIPTEN)
    target_compile_definitions(recompinput PRIVATE RECOMPINPUT_SINGLE_PLAYER_UNTIL_ASSIGNED=1)
endif()
