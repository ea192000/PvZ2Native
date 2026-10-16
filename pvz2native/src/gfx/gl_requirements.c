/* Host OpenGL capability detection and the startup requirement gate.
 *
 * See include/pvz2native/gfx/gl_requirements.h for why the floor is "OpenGL 2.0
 * with framebuffer objects" and why this module -- not the GL shim -- decides
 * the GLSL dialect. Kept in its own translation unit so the policy lives in one
 * place and main.c only has to ask "may I start?".
 */

#include <pvz2native/gfx/gl_requirements.h>
#include <pvz2native/log.h>

#include <glad/gl.h>
#include <SDL.h>

#include <ctype.h>
#include <stdio.h>

/* Latched by pvz2_gl_check_requirements(). 130 is the safe default so that any
 * code asking before detection has run gets the already-proven 3.0 path. */
static int g_target_glsl = 130;

/* Pulls "MAJOR.MINOR" out of a GL_VERSION string. The desktop form starts with
 * the number ("3.0.0 - Build ...", "4.6.0 NVIDIA ..."); an "OpenGL ES 3.0"-style
 * prefix is skipped by scanning to the first digit first. Leaves *major/*minor
 * untouched (0) when nothing parses. */
static void parse_gl_version(const char *s, int *major, int *minor) {
    *major = 0;
    *minor = 0;
    if (s == NULL) return;
    while (*s != '\0' && !isdigit((unsigned char)*s)) ++s;
    if (*s == '\0') return;
    int hi = 0, lo = 0;
    if (sscanf(s, "%d.%d", &hi, &lo) >= 1) {
        *major = hi;
        *minor = lo;
    }
}

/* The engine binds an offscreen framebuffer every frame; without these entry
 * points loaded (core in 3.0, ARB/EXT_framebuffer_object on 2.1) the render
 * path cannot work, so this is part of the requirement, not a nicety. */
static int has_framebuffer_objects(void) {
    return glGenFramebuffers != NULL && glBindFramebuffer != NULL &&
           glFramebufferTexture2D != NULL && glCheckFramebufferStatus != NULL;
}

int pvz2_gl_target_glsl(void) { return g_target_glsl; }

int pvz2_gl_check_requirements(struct SDL_Window *window) {
    const char *version  = (const char *)glGetString(GL_VERSION);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *vendor   = (const char *)glGetString(GL_VENDOR);
    if (version == NULL)  version  = "unknown";
    if (renderer == NULL) renderer = "unknown";
    if (vendor == NULL)   vendor   = "unknown";

    int major = 0, minor = 0;
    parse_gl_version(version, &major, &minor);

    /* A 3.0+ context takes the proven "#version 130" path; a 2.x one needs the
     * 1.20 rewrite. Decided here, read by the GL shim via pvz2_gl_target_glsl. */
    g_target_glsl = (major >= 3) ? 130 : 120;

    const int version_ok =
        major > PVZ2_REQUIRED_GL_MAJOR ||
        (major == PVZ2_REQUIRED_GL_MAJOR && minor >= PVZ2_REQUIRED_GL_MINOR);
    const int fbo_ok = has_framebuffer_objects();

    if (version_ok && fbo_ok) {
        pvz2_log("[gl] GPU OK -- %s (%s), OpenGL %s -> GLSL %d", renderer, vendor, version,
               g_target_glsl);
        return 1;
    }

    /* Say exactly what failed: an old version and a missing FBO capability are
     * different problems (the second can happen on a barely-2.0 driver), and the
     * user can only act on the one that is actually true. */
    char detail[256];
    if (!version_ok) {
        snprintf(detail, sizeof(detail), "Detected OpenGL: %s", version);
    } else {
        snprintf(detail, sizeof(detail),
                 "Detected OpenGL: %s\n(no framebuffer object support)", version);
    }

    char message[768];
    snprintf(message, sizeof(message),
             "Your GPU does not support the OpenGL this game requires.\n\n"
             "GPU: %s\n"
             "Vendor: %s\n"
             "%s\n"
             "Required: OpenGL %d.%d or newer (with framebuffer objects)\n\n"
             "Try updating your graphics drivers. A GPU that cannot provide\n"
             "OpenGL 2.0 cannot run the game -- it has no programmable shaders.",
             renderer, vendor, detail, PVZ2_REQUIRED_GL_MAJOR, PVZ2_REQUIRED_GL_MINOR);

    pvz2_log("[gl] requirement not met -- %s", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PvZ2 - Unsupported GPU", message,
                             (SDL_Window *)window);
    return 0;
}
