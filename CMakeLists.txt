cmake_minimum_required(VERSION 3.5)
# These policies exist only on newer CMake (3.27+ / 3.30+). Ubuntu 20.04 ships
# 3.16, so guard them: without the guard, configure aborts before the build.
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()
project(pvz2native)

include_directories(${PROJECT_SOURCE_DIR}/stb)

SET(SDL_STATIC ON CACHE BOOL "Enable SDL_STATIC" FORCE)
SET(SDL_SHARED OFF CACHE BOOL "Disable SDL_SHARED" FORCE)
SET(SDL_MMX OFF CACHE BOOL "Disable SDL_MMX" FORCE)
SET(SDL_SSE OFF CACHE BOOL "Disable SDL_SSE" FORCE)
SET(SDL_SSE2 OFF CACHE BOOL "Disable SDL_SSE2" FORCE)
SET(SDL_SSE3 OFF CACHE BOOL "Disable SDL_SSE3" FORCE)
SET(SDL_SSEMATH OFF CACHE BOOL "Disable SDL_SSEMATH" FORCE)
SET(SDL_3DNOW OFF CACHE BOOL "Disable SDL_3DNOW" FORCE)
SET(SDL_TEST OFF CACHE BOOL "Disable SDL_TEST" FORCE)
SET(SDL_TESTS OFF CACHE BOOL "Disable SDL_TESTS" FORCE)

add_subdirectory(glad/cmake)
glad_add_library(glad REPRODUCIBLE LOADER API egl=1.5 gl:compatibility=2.0)
add_subdirectory(zlib)
add_subdirectory(SDL)

# PvZ2 is armeabi-v7a (32-bit ARM) only -- no need for the A64 frontend.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Selects which frontends to enable" FORCE)
add_subdirectory(dynarmic)

add_subdirectory(pvz2native)
