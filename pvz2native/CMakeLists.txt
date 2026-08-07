cmake_minimum_required (VERSION 3.5)

project(pvz2native-core)

file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS src/*.c src/*.cpp)

# stb_vorbis is gone with src/audio/sound_repository.c, its only consumer. That
# file, src/audio/sles.c and src/audio/audio_engine.c were an older OpenSL
# implementation with no callers left: the live audio path is the guest-vtable
# bridge in src/dependencies/libopensles.cpp plus src/audio/audio_output.cpp.
if (WIN32)
    add_executable(pvz2native WIN32 ${SOURCE_FILES})
    target_sources(pvz2native PRIVATE windows_resources.rc)
else()
    add_executable(pvz2native ${SOURCE_FILES})
endif()
target_include_directories(pvz2native PUBLIC include/ ../stb/)
target_link_libraries(pvz2native ${CMAKE_DL_LIBS} zlibstatic SDL2::SDL2 SDL2main glad dynarmic)
include_directories(pvz2native PUBLIC include/)
set_target_properties(pvz2native PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON OUTPUT_NAME pvz2)
if (MSVC)
    target_compile_options(pvz2native PRIVATE /utf-8)
endif()

# Link the MinGW runtime statically so the .exe runs on a machine that has no
# toolchain installed. Everything else here is already static (SDL_STATIC is
# forced on in the root CMakeLists, as are zlib/glad/dynarmic), so the only
# remaining runtime dependencies were the three GCC DLLs -- and copying the
# binary to another PC failed on exactly one of them, libwinpthread-1.dll,
# which std::thread and std::mutex pull in.
#
# 64-bit on purpose, and it has to stay that way: dynarmic ships host backends
# for x86_64, arm64 and riscv64 only. Building with -m32 makes its
# DetectArchitecture report "x86", no backend sources are compiled at all, and
# the link fails on every Dynarmic::A32::Jit symbol. A static x86_64 build runs
# on any 64-bit Windows anyway, which is what -m32 would have been for.
if (MINGW)
    target_link_options(pvz2native PRIVATE -static)
endif()
