cmake_minimum_required(VERSION 3.20)
project(cmrally C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 REQUIRED CONFIG)

set(GEN_DIR ${CMAKE_SOURCE_DIR}/generated CACHE PATH "Recompiler output directory")
if(NOT EXISTS ${GEN_DIR}/sources.cmake)
  message(FATAL_ERROR "No recompiled code in ${GEN_DIR}. Run: python3 tools/build_game.py <path to game .cue>")
endif()
include(${GEN_DIR}/sources.cmake)

set(RUNTIME_SOURCES
  runtime/main.c runtime/core.c runtime/mem.c runtime/bios.c runtime/timers.c
  runtime/gpu.c runtime/gte.c runtime/spu.c runtime/cdrom.c runtime/mdec.c runtime/sio.c
  runtime/render_gl.c runtime/pgxp.c runtime/hooks.c)

# Recompiled game code: large, machine-generated functions.
add_library(game_code STATIC ${GENERATED_SOURCES})
target_include_directories(game_code PRIVATE runtime ${GEN_DIR})
target_compile_options(game_code PRIVATE -O1 -w -fno-strict-aliasing)
add_executable(cmrally ${RUNTIME_SOURCES})
target_include_directories(cmrally PRIVATE runtime)
target_compile_options(cmrally PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
target_link_libraries(cmrally PRIVATE game_code SDL3::SDL3)

option(RECOMP_TRACE "Record recently entered game functions for debugging" ON)
if(RECOMP_TRACE)
  target_compile_definitions(game_code PRIVATE RECOMP_TRACE)
  target_compile_definitions(cmrally PRIVATE RECOMP_TRACE)
endif()
