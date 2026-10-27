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
  runtime/render_gpu.c runtime/pgxp.c runtime/race60.c runtime/view.c runtime/unlock.c runtime/cars.c)

# Recompiled game code: large, machine-generated functions.
add_library(game_code STATIC ${GENERATED_SOURCES})
target_include_directories(game_code PRIVATE runtime ${GEN_DIR})
if(MSVC)
  target_compile_options(game_code PRIVATE /O1 /w /bigobj)
else()
  target_compile_options(game_code PRIVATE -O1 -w -fno-strict-aliasing)
endif()
add_executable(cmrally ${RUNTIME_SOURCES})
target_include_directories(cmrally PRIVATE runtime)
if(MSVC)
  target_compile_options(cmrally PRIVATE /W3)
  target_compile_definitions(cmrally PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(cmrally PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
  target_link_libraries(cmrally PRIVATE m)
endif()
target_link_libraries(cmrally PRIVATE game_code SDL3::SDL3)

# The call-stack trace relies on __attribute__((cleanup)), available in GCC and Clang.
if(MSVC)
  set(trace_default OFF)
else()
  set(trace_default ON)
endif()
option(RECOMP_TRACE "Record recently entered game functions for debugging" ${trace_default})
if(RECOMP_TRACE)
  target_compile_definitions(game_code PRIVATE RECOMP_TRACE)
  target_compile_definitions(cmrally PRIVATE RECOMP_TRACE)
endif()

# Local regression test against your own disc; record references from a good build first:
#   python3 tools/regress.py build/cmrally record
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME regress COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/regress.py $<TARGET_FILE:cmrally> check)
  set_tests_properties(regress PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
  add_test(NAME car-files COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/cmrcar_test.py)
  set_tests_properties(car-files PROPERTIES SKIP_RETURN_CODE 77)
endif()
