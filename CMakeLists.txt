cmake_minimum_required(VERSION 3.24)

project(benefactor-port LANGUAGES C CXX)

if(MINGW)
    # GitHub's MinGW environment can inherit MSVC-style linker flags from its
    # toolchain setup. GNU ld treats those as input paths, so remove them at
    # the project boundary while retaining the user's other linker options.
    foreach(linker_flags CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                          CMAKE_MODULE_LINKER_FLAGS)
        foreach(msvc_flag /OPT:REF /OPT:ICF /INCREMENTAL:NO -OPT:REF -OPT:ICF
                           -INCREMENTAL:NO)
            string(REPLACE "${msvc_flag}" "" ${linker_flags}
                   "${${linker_flags}}")
        endforeach()
    endforeach()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BENEFACTOR_AMIGAPORT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../shared/amigaport")
if(DEFINED ENV{BENEFACTOR_AMIGAPORT_DIR} AND NOT "$ENV{BENEFACTOR_AMIGAPORT_DIR}" STREQUAL "")
    set(BENEFACTOR_AMIGAPORT_DEFAULT "$ENV{BENEFACTOR_AMIGAPORT_DIR}")
endif()
set(BENEFACTOR_AMIGAPORT_DIR "${BENEFACTOR_AMIGAPORT_DEFAULT}"
    CACHE PATH "Path to the shared amigaport runtime")
get_filename_component(BENEFACTOR_AMIGAPORT_DIR "${BENEFACTOR_AMIGAPORT_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT IS_DIRECTORY "${BENEFACTOR_AMIGAPORT_DIR}")
    message(FATAL_ERROR
        "Benefactor requires shared/amigaport at ${BENEFACTOR_AMIGAPORT_DIR}; "
        "provide -DBENEFACTOR_AMIGAPORT_DIR=<checkout>")
endif()
set(AMIGAPORT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${BENEFACTOR_AMIGAPORT_DIR}" "${CMAKE_BINARY_DIR}/amigaport")

# lucent — the logging, configuration and loopback HTTP control channel this
# product had hand-rolled three times over. Same checkout convention as
# amigaport: a sibling directory, overridable for CI.
set(BENEFACTOR_LUCENT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../lucent")
if(DEFINED ENV{BENEFACTOR_LUCENT_DIR} AND NOT "$ENV{BENEFACTOR_LUCENT_DIR}" STREQUAL "")
    set(BENEFACTOR_LUCENT_DEFAULT "$ENV{BENEFACTOR_LUCENT_DIR}")
endif()
set(BENEFACTOR_LUCENT_DIR "${BENEFACTOR_LUCENT_DEFAULT}"
    CACHE PATH "Path to the lucent library")
get_filename_component(BENEFACTOR_LUCENT_DIR "${BENEFACTOR_LUCENT_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT IS_DIRECTORY "${BENEFACTOR_LUCENT_DIR}")
    message(FATAL_ERROR
        "Benefactor requires lucent at ${BENEFACTOR_LUCENT_DIR}; "
        "provide -DBENEFACTOR_LUCENT_DIR=<checkout>")
endif()
set(LUCENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(ANDROID OR EMSCRIPTEN)
    set(LUCENT_BUILD_ZIP OFF CACHE BOOL "" FORCE)
else()
    set(LUCENT_BUILD_ZIP ON CACHE BOOL "" FORCE)
    if(MINGW)
        set(ZLIB_USE_STATIC_LIBS ON)
    endif()
endif()
add_subdirectory("${BENEFACTOR_LUCENT_DIR}" "${CMAKE_BINARY_DIR}/lucent")

if(ANDROID)
    set(BENEFACTOR_SDL3_PREFIX "" CACHE PATH "Shared Android-port SDL3 prefix")
    if(NOT IS_DIRECTORY "${BENEFACTOR_SDL3_PREFIX}")
        message(FATAL_ERROR "Android requires -DBENEFACTOR_SDL3_PREFIX=<directory>")
    endif()
    set(SDL3_DIR "${BENEFACTOR_SDL3_PREFIX}/lib/cmake/SDL3" CACHE PATH
        "Shared Android-port SDL3 CMake package")
    find_package(SDL3 CONFIG REQUIRED)
    set(BENEFACTOR_SDL_TARGET SDL3::SDL3)
else()
    set(BENEFACTOR_SDL3_DIR "$ENV{BENEFACTOR_SDL3_DIR}" CACHE PATH
        "Optional SDL3 source checkout")
    if(IS_DIRECTORY "${BENEFACTOR_SDL3_DIR}")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
        add_subdirectory("${BENEFACTOR_SDL3_DIR}" "${CMAKE_BINARY_DIR}/sdl3" EXCLUDE_FROM_ALL)
        if(TARGET SDL3::SDL3-static)
            set(BENEFACTOR_SDL_TARGET SDL3::SDL3-static)
        elseif(TARGET SDL3::SDL3)
            set(BENEFACTOR_SDL_TARGET SDL3::SDL3)
        else()
            message(FATAL_ERROR "SDL3 checkout did not expose a usable CMake target")
        endif()
    else()
        find_package(SDL3 CONFIG REQUIRED)
        if(TARGET SDL3::SDL3)
            set(BENEFACTOR_SDL_TARGET SDL3::SDL3)
        else()
            message(FATAL_ERROR "SDL3 package did not expose SDL3::SDL3")
        endif()
    endif()
endif()

find_package(Threads REQUIRED)

set(BENEFACTOR_SOURCES
    src/main.c
    src/common/log.c
    src/engine/disk_boot.c
    src/engine/gameplay_handoff.c
    src/engine/hw.c
    src/engine/hw_beam.c
    src/engine/hw_audio.c
    src/engine/hw_blitter.c
    src/engine/hw_testrun.c
    src/engine/hw_watchdog.c
    src/engine/overlay_load.c
    src/port/config.c
    src/port/freecam.c
    src/port/frame_accounting.cpp
    src/port/frame_signature.c
    src/port/lockstep.c
    src/port/state_dump.c
    src/port/guest_profile.cpp
    src/port/game_loop.c
    src/port/savestate.c
    src/port/guest_vectors.c
    src/port/level_layout.c
    src/port/guest_trace.c
    src/port/control/control_server.cpp
    src/port/control/input_script.cpp
    src/port/debug/debugger.cpp
    src/port/hud_icons.c
    src/port/input.c
    src/port/level_select_ui.c
    src/port/overrides/audio.c
    src/port/overrides/boot.c
    src/port/overrides/copper.c
    src/port/overrides/gameplay.c
    src/port/overrides/level_load.c
    src/port/overrides/hw.c
    src/port/overrides/pickup.c
    src/port/overrides/platformer.c
    src/port/overrides/register.c
    src/port/overrides/wait_idioms.c
    src/port/overrides/render.c
    src/port/pause_menu.c
    src/port/profile.c
    src/port/project_paths.c
    src/render/engine_view.c
    src/render/native_effects.c
    src/render/native_renderer.c
    src/render/present_sdl.c
    src/render/scene.c
    src/render/scene_sdl.c
    src/runtime/guest_call_policy.cpp
    src/runtime/guest_runtime.cpp)

if(NOT ANDROID AND NOT BENEFACTOR_WEB)
    list(APPEND BENEFACTOR_SOURCES
        src/platform/desktop_setup.cpp
        src/platform/disk_selection_store.cpp)
endif()

if(BENEFACTOR_WEB)
    list(REMOVE_ITEM BENEFACTOR_SOURCES src/main.c)
    list(APPEND BENEFACTOR_SOURCES src/platform/web_main.c)
endif()

if(BENEFACTOR_WEB)
    add_executable(benefactor_web ${BENEFACTOR_SOURCES})
    set_target_properties(benefactor_web PROPERTIES OUTPUT_NAME benefactor)
elseif(ANDROID)
    list(APPEND BENEFACTOR_SOURCES
        src/platform/android_bridge.c
        src/port/touch_controls.cpp)
    add_library(benefactor-pc SHARED ${BENEFACTOR_SOURCES})
    set_target_properties(benefactor-pc PROPERTIES OUTPUT_NAME main)
else()
    add_executable(benefactor-pc ${BENEFACTOR_SOURCES})
    if(APPLE)
        set_target_properties(benefactor-pc PROPERTIES
            MACOSX_BUNDLE TRUE
            OUTPUT_NAME Benefactor)
    endif()
endif()

if(BENEFACTOR_WEB)
    set(BENEFACTOR_PRODUCT_TARGET benefactor_web)
else()
    set(BENEFACTOR_PRODUCT_TARGET benefactor-pc)
endif()

target_include_directories(${BENEFACTOR_PRODUCT_TARGET} PRIVATE src)
target_link_libraries(${BENEFACTOR_PRODUCT_TARGET} PRIVATE
    amigaport::amigaport
    lucent::lucent
    ${BENEFACTOR_SDL_TARGET}
    Threads::Threads)
if(NOT ANDROID AND NOT BENEFACTOR_WEB)
    target_link_libraries(${BENEFACTOR_PRODUCT_TARGET} PRIVATE lucent::zip)
endif()
if(NOT WIN32 AND NOT BENEFACTOR_WEB)
    target_link_libraries(${BENEFACTOR_PRODUCT_TARGET} PRIVATE m)
endif()
if(WIN32)
    target_link_libraries(${BENEFACTOR_PRODUCT_TARGET} PRIVATE ws2_32)
endif()
if(ANDROID)
    target_link_libraries(benefactor-pc PRIVATE android log)
    target_compile_definitions(benefactor-pc PRIVATE BENEFACTOR_ANDROID)
endif()
if(BENEFACTOR_WEB)
    target_compile_definitions(benefactor_web PRIVATE BENEFACTOR_WEB)
    target_link_options(benefactor_web PRIVATE
        "-sALLOW_MEMORY_GROWTH=1"
        "-sNO_EXIT_RUNTIME=1"
        "-sEXPORTED_FUNCTIONS=['_main','_benefactor_web_start']"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','FS']")
elseif(NOT MSVC)
    target_compile_options(${BENEFACTOR_PRODUCT_TARGET} PRIVATE -Wall -Wextra)
endif()

if(NOT ANDROID AND NOT BENEFACTOR_WEB)
    include(GNUInstallDirs)
    install(TARGETS benefactor-pc
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES platforms/freedesktop/io.github.SomeoneIsWorking.benefactor.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES platforms/freedesktop/io.github.SomeoneIsWorking.benefactor.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

add_custom_target(benefactor_product DEPENDS ${BENEFACTOR_PRODUCT_TARGET})

enable_testing()
find_program(BENEFACTOR_UV_EXECUTABLE uv)
if(BENEFACTOR_UV_EXECUTABLE)
    add_test(
        NAME benefactor_source_policy
        COMMAND "${BENEFACTOR_UV_EXECUTABLE}" run --frozen python -m tools.verify)
    set_tests_properties(benefactor_source_policy PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
