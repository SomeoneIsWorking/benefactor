cmake_minimum_required(VERSION 3.24)

project(benefactor-port LANGUAGES C CXX)

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

if(ANDROID)
    set(BENEFACTOR_SDL2_DIR "" CACHE PATH "SDL2 source checkout for Android")
    set(BENEFACTOR_LUCENT_DIR "" CACHE PATH "Lucent source checkout for Android")
    foreach(required_dir BENEFACTOR_SDL2_DIR BENEFACTOR_LUCENT_DIR)
        if(NOT IS_DIRECTORY "${${required_dir}}")
            message(FATAL_ERROR "Android requires -D${required_dir}=<source checkout>")
        endif()
    endforeach()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BENEFACTOR_SDL2_DIR}" "${CMAKE_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)
    add_subdirectory("${BENEFACTOR_LUCENT_DIR}" "${CMAKE_BINARY_DIR}/lucent" EXCLUDE_FROM_ALL)
    if(TARGET SDL2::SDL2)
        set(BENEFACTOR_SDL_TARGET SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        set(BENEFACTOR_SDL_TARGET SDL2::SDL2-static)
    else()
        message(FATAL_ERROR "SDL2 checkout did not expose SDL2::SDL2 or SDL2::SDL2-static")
    endif()
else()
    set(BENEFACTOR_SDL2_DIR "$ENV{BENEFACTOR_SDL2_DIR}" CACHE PATH
        "Optional SDL2 source checkout")
    if(IS_DIRECTORY "${BENEFACTOR_SDL2_DIR}")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        add_subdirectory("${BENEFACTOR_SDL2_DIR}" "${CMAKE_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)
        if(TARGET SDL2::SDL2-static)
            set(BENEFACTOR_SDL_TARGET SDL2::SDL2-static)
        elseif(TARGET SDL2::SDL2)
            set(BENEFACTOR_SDL_TARGET SDL2::SDL2)
        else()
            message(FATAL_ERROR "SDL2 checkout did not expose a usable CMake target")
        endif()
    else()
        find_package(SDL2 REQUIRED)
        if(TARGET SDL2::SDL2)
            set(BENEFACTOR_SDL_TARGET SDL2::SDL2)
        else()
            set(BENEFACTOR_SDL_TARGET ${SDL2_LIBRARIES})
        endif()
    endif()
endif()

find_package(Threads REQUIRED)

set(BENEFACTOR_SOURCES
    src/main.c
    src/common/log.c
    src/engine/disk_boot.c
    src/engine/hw.c
    src/engine/hw_audio.c
    src/engine/hw_blitter.c
    src/engine/overlay_load.c
    src/port/config.c
    src/port/freecam.c
    src/port/game_loop.c
    src/port/http_debug.c
    src/port/hud_icons.c
    src/port/input.c
    src/port/level_select_ui.c
    src/port/overrides/audio.c
    src/port/overrides/boot.c
    src/port/overrides/copper.c
    src/port/overrides/gameplay.c
    src/port/overrides/hw.c
    src/port/overrides/pickup.c
    src/port/overrides/platformer.c
    src/port/overrides/register.c
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
    src/runtime/guest_runtime.cpp)

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
    ${BENEFACTOR_SDL_TARGET}
    Threads::Threads)
if(NOT WIN32 AND NOT BENEFACTOR_WEB)
    target_link_libraries(${BENEFACTOR_PRODUCT_TARGET} PRIVATE m)
endif()
if(ANDROID)
    target_link_libraries(benefactor-pc PRIVATE lucent::lucent android log)
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
