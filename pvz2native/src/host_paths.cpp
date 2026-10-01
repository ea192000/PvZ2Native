/* host_paths.cpp — install vs data directory (AppImage-aware on Linux). */

#include <pvz2native/host_paths.h>

#include <SDL.h>

#include <cstring>

#ifndef _WIN32
#include <cstdlib>
#endif

extern "C" char *pvz2_host_install_path(void) {
    return SDL_GetBasePath();
}

extern "C" char *pvz2_host_data_path(void) {
#ifdef _WIN32
    return SDL_GetBasePath();
#else
    /* AppImage runtime sets APPIMAGE to the path of the .AppImage file. When
     * unset (loose binary, or running usr/bin/pvz2 directly), behave exactly
     * as before: everything next to the executable. */
    const char *appimage = std::getenv("APPIMAGE");
    if (!appimage || !appimage[0])
        return SDL_GetBasePath();

    const char *slash = std::strrchr(appimage, '/');
    if (!slash)
        return SDL_GetBasePath();

    /* dirname + trailing separator, SDL_free-compatible. */
    const size_t dir_len = (size_t)(slash - appimage) + 1; /* include '/' */
    char *out = (char *)SDL_malloc(dir_len + 1);
    if (!out)
        return SDL_GetBasePath();
    std::memcpy(out, appimage, dir_len);
    out[dir_len] = '\0';
    return out;
#endif
}
