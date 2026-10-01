/* achievements.cpp — local achievements: persist, popup toast, scrollable list.
 *
 * Persistence: Google Play service IDs stored as newline-joined string in
 * prefs.dat key "pvz2_native_completed_achievements" via config_store.
 *
 * Popup: slides up from bottom, holds, slides down.  Queue for sequential display.
 * List: full-screen overlay with darken, centered menu panel, scrollable items.
 *
 * All GL work runs on the main thread inside achievements_draw().
 */

#include <pvz2native/achievements.h>
#include <pvz2native/achievement_catalog.h>
#include <pvz2native/config.h>
#include <pvz2native/config_screen.h>
#include <pvz2native/host_assets.h>
#include <pvz2native/dex/config_store.h>

#include "stb_truetype.h"

#include <SDL.h>
#include <glad/gl.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace pvz2native::achievements;

/* =========================================================================
 * Forward declarations from config_screen.h
 * ========================================================================= */
extern "C" void apply_magenta_colorkey(SDL_Surface *surface);

/* =========================================================================
 * Persistence (thread-safe, shared key with google_play.cpp)
 * ========================================================================= */
static const char *kAchievementsKey = "pvz2_native_completed_achievements";

static std::mutex g_store_mtx;
static std::set<std::string> g_completed;
static bool g_store_loaded = false;

static void store_load_locked() {
    if (g_store_loaded) return;
    g_store_loaded = true;
    std::string blob;
    if (!pvz2native::dex::config_store_get_string(kAchievementsKey, blob)) return;
    std::size_t start = 0;
    while (start < blob.size()) {
        const std::size_t nl = blob.find('\n', start);
        const std::size_t end = (nl == std::string::npos) ? blob.size() : nl;
        if (end > start) g_completed.insert(blob.substr(start, end - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

static void store_save_locked() {
    std::string blob;
    for (const std::string &id : g_completed) { blob += id; blob += '\n'; }
    pvz2native::dex::config_store_set_string(kAchievementsKey, blob);
}

/* Returns catalog index for a catalog_id string, or -1. */
static int find_catalog_index(const char *catalog_id) {
    if (!catalog_id) return -1;
    for (int i = 0; i < kAchievementCount; ++i)
        if (std::strcmp(kAchievements[i].id, catalog_id) == 0) return i;
    return -1;
}

/* Map Google Play service id → catalog id via kAchievementServiceMap. */
static const char *service_to_catalog(const char *service_id) {
    if (!service_id) return nullptr;
    for (int i = 0; i < kAchievementServiceMapCount; ++i)
        if (std::strcmp(kAchievementServiceMap[i].service_id, service_id) == 0)
            return kAchievementServiceMap[i].catalog_id;
    return nullptr;
}

/* =========================================================================
 * Localization
 * ========================================================================= */
static AchievementLang locale_to_lang(const char *loc) {
    if (!loc || !loc[0]) return kLangEN;
    if (std::strncmp(loc, "de", 2) == 0) return kLangDE;
    if (std::strncmp(loc, "es", 2) == 0) return kLangES;
    if (std::strncmp(loc, "fr", 2) == 0) return kLangFR;
    if (std::strncmp(loc, "it", 2) == 0) return kLangIT;
    if (std::strncmp(loc, "pt", 2) == 0) return kLangPT;
    return kLangEN;
}

static AchievementLang current_lang() {
    const pvz2_config_t *cfg = pvz2_config();
    return locale_to_lang(cfg ? cfg->user_locale : nullptr);
}

static const char *screen_title_for_lang(AchievementLang lang) {
    static const char *kTitles[kLangCount] = {
        "Erfolge",       /* DE */
        "Achievements",  /* EN */
        "Logros",        /* ES */
        "Succ\xc3\xa8s",      /* FR */
        "Obiettivi",     /* IT */
        "Conquistas",    /* PT */
    };
    return kTitles[lang];
}

/* =========================================================================
 * Texture helpers (copied pattern from config_screen.cpp)
 * ========================================================================= */
struct Tex {
    GLuint id = 0;
    int w = 0, h = 0;
};

static Tex load_bmp_from_rc(int rc_id) {
    Tex t;
    SDL_RWops *rw = nullptr;
    void *file_buf = nullptr;
#ifdef _WIN32
    HMODULE self = GetModuleHandle(NULL);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(rc_id), RT_RCDATA);
    if (!res) return t;
    HGLOBAL h = LoadResource(self, res);
    if (!h) return t;
    DWORD bytes = SizeofResource(self, res);
    const void *ptr = LockResource(h);
    if (!ptr || !bytes) return t;
    rw = SDL_RWFromConstMem(ptr, (int)bytes);
#else
    int bytes = 0;
    file_buf = pvz2_load_asset_bytes(rc_id, &bytes);
    if (!file_buf || bytes <= 0) { if (file_buf) SDL_free(file_buf); return t; }
    rw = SDL_RWFromConstMem(file_buf, bytes);
#endif
    if (!rw) { if (file_buf) SDL_free(file_buf); return t; }
    SDL_Surface *bmp = SDL_LoadBMP_RW(rw, 1);
    if (file_buf) SDL_free(file_buf);
    if (!bmp) return t;
    SDL_Surface *surf = SDL_ConvertSurfaceFormat(bmp, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(bmp);
    if (!surf) return t;
    apply_magenta_colorkey(surf);
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    int bpp = surf->format->BytesPerPixel;
    if (bpp > 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, surf->pitch / bpp);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    t.w = surf->w; t.h = surf->h;
    SDL_FreeSurface(surf);
    return t;
}

static void *load_raw_from_rc(int rc_id, int *out_size) {
#ifdef _WIN32
    HMODULE self = GetModuleHandle(NULL);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(rc_id), RT_RCDATA);
    if (!res) return nullptr;
    HGLOBAL h = LoadResource(self, res);
    if (!h) return nullptr;
    DWORD bytes = SizeofResource(self, res);
    const void *ptr = LockResource(h);
    if (!ptr || !bytes) return nullptr;
    void *buf = SDL_malloc(bytes);
    SDL_memcpy(buf, ptr, bytes);
    if (out_size) *out_size = (int)bytes;
    return buf;
#else
    return pvz2_load_asset_bytes(rc_id, out_size);
#endif
}

static void free_tex(Tex &t) {
    if (t.id) { glDeleteTextures(1, &t.id); t.id = 0; }
    t.w = t.h = 0;
}

/* =========================================================================
 * Font (stb_truetype, same as config_screen)
 * ========================================================================= */
struct AchFont {
    stbtt_fontinfo info{};
    void *data = nullptr;
    GLuint atlas_tex = 0;
    static const int kAtlasW = 2048;
    static const int kAtlasH = 2048;
    unsigned char atlas_buf[kAtlasW * kAtlasH]{};
    stbtt_bakedchar glyphs_header[224]{}; /* 92px bake */
    stbtt_bakedchar glyphs_item[224]{};   /* 48px bake */
    float scale_header = 0.0f;
    float scale_item   = 0.0f;
    int ascent_unscaled = 0;
    bool ok = false;

    bool load(int rc_id, float px_header, float px_item) {
        int sz = 0;
        data = load_raw_from_rc(rc_id, &sz);
        if (!data) return false;
        if (!stbtt_InitFont(&info, (unsigned char *)data, 0)) return false;
        scale_header = stbtt_ScaleForPixelHeight(&info, px_header);
        scale_item   = stbtt_ScaleForPixelHeight(&info, px_item);
        int asc = 0, desc = 0, lg = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &lg);
        ascent_unscaled = asc;
        stbtt_BakeFontBitmap((unsigned char *)data, 0, px_header,
                             atlas_buf, kAtlasW, kAtlasH / 2, 32, 224, glyphs_header);
        stbtt_BakeFontBitmap((unsigned char *)data, 0, px_item,
                             atlas_buf + kAtlasW * (kAtlasH / 2), kAtlasW, kAtlasH / 2,
                             32, 224, glyphs_item);
        for (int i = 0; i < 224; ++i) {
            glyphs_item[i].y0 += (short)(kAtlasH / 2);
            glyphs_item[i].y1 += (short)(kAtlasH / 2);
        }
        glGenTextures(1, &atlas_tex);
        glBindTexture(GL_TEXTURE_2D, atlas_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, kAtlasW, kAtlasH, 0,
                     GL_ALPHA, GL_UNSIGNED_BYTE, atlas_buf);
        ok = true;
        return true;
    }

    void unload() {
        if (atlas_tex) { glDeleteTextures(1, &atlas_tex); atlas_tex = 0; }
        if (data) { SDL_free(data); data = nullptr; }
        ok = false;
    }
};

/* =========================================================================
 * UTF-8 helper
 * ========================================================================= */
static unsigned int utf8_next(const char **p) {
    unsigned char c = (unsigned char)**p;
    if (!c) return 0;
    ++(*p);
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) {
        unsigned char c2 = (unsigned char)**p;
        if ((c2 & 0xC0) != 0x80) return '?';
        ++(*p);
        return ((unsigned int)(c & 0x1F) << 6) | (c2 & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {
        unsigned char c2 = (unsigned char)**p, c3;
        if ((c2 & 0xC0) != 0x80) return '?';
        ++(*p); c3 = (unsigned char)**p;
        if ((c3 & 0xC0) != 0x80) return '?';
        ++(*p);
        return ((unsigned int)(c & 0x0F) << 12) | ((unsigned int)(c2 & 0x3F) << 6) | (c3 & 0x3F);
    }
    return '?';
}

/* =========================================================================
 * Scale helpers
 * ========================================================================= */
static const float kRefH = 1080.0f;

/* Menu panel scale: height-based, clamped so menu fits in width. */
static float menu_scale(float win_w, float win_h) {
    float s = win_h / kRefH;
    if (1415.0f * s > win_w) s = win_w / 1415.0f;
    return s;
}

/* Active UI scale for the current draw pass (popup may differ from menu). */
static float g_ui_scale = 1.0f;

static float sc(float ref, float scale) { return ref * scale; }

/* Alpha at a Y position inside the list: 0 in the solid rim, ramp through the
 * fade band, 1 in the flat content area (same idea as config_screen). */
static float list_edge_alpha(float y, float list_top, float list_bot,
                             float solid, float fade_end) {
    float span = fade_end - solid;
    if (span <= 0.0f) return 1.0f;
    float a = 1.0f;
    float d_top = y - list_top;
    if (d_top < fade_end) {
        if (d_top <= solid) return 0.0f;
        a = (d_top - solid) / span;
    }
    float d_bot = list_bot - y;
    if (d_bot < fade_end) {
        if (d_bot <= solid) return 0.0f;
        float ab = (d_bot - solid) / span;
        if (ab < a) a = ab;
    }
    return a;
}

/* =========================================================================
 * Text drawing
 * ========================================================================= */
static float measure_string(AchFont &f, const char *text, bool is_header,
                            float win_w, float win_h, float font_mul = 1.0f) {
    (void)win_w; (void)win_h;
    if (!f.ok || !text) return 0;
    const float scale = g_ui_scale * (font_mul > 0.0f ? font_mul : 1.0f);
    stbtt_bakedchar *bc = is_header ? f.glyphs_header : f.glyphs_item;
    float w = 0;
    const char *p = text;
    unsigned int cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp < 32 || cp > 255) { w += 8.0f * scale; continue; }
        w += bc[cp - 32].xadvance * scale;
    }
    return w + 1.0f * scale;
}

static float draw_string_ach(AchFont &font, const char *text, bool is_header,
                         float x, float y,
                         float cr, float cg, float cb,
                         float win_w, float win_h,
                         bool shadow = false, float shadow_dx = 0, float shadow_dy = 0,
                         float outline_px = 0, float shadow_alpha = 0.25f,
                         float text_alpha = 1.0f, float font_mul = 1.0f,
                         float fade_list_top = 0, float fade_list_bot = 0,
                         float fade_solid = 0, float fade_end = 0) {
    if (!font.ok || !text || text_alpha <= 0.0f) return x;
    (void)outline_px; (void)win_w; (void)win_h;
    const bool do_fade = (fade_end > fade_solid);

    const float scale = g_ui_scale * (font_mul > 0.0f ? font_mul : 1.0f);

    glBindTexture(GL_TEXTURE_2D, font.atlas_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    stbtt_bakedchar *bc = is_header ? font.glyphs_header : font.glyphs_item;
    float fw = (float)AchFont::kAtlasW;
    float fh = (float)AchFont::kAtlasH;

    auto emit_glyph = [&](float px, float py, stbtt_bakedchar *cd, float r, float g, float b, float a) {
        float x0 = px + cd->xoff * scale;
        float y0 = py + cd->yoff * scale;
        float x1 = x0 + (cd->x1 - cd->x0) * scale;
        float y1 = y0 + (cd->y1 - cd->y0) * scale;
        float at = a, ab = a;
        if (do_fade) {
            at *= list_edge_alpha(y0, fade_list_top, fade_list_bot, fade_solid, fade_end);
            ab *= list_edge_alpha(y1, fade_list_top, fade_list_bot, fade_solid, fade_end);
            if (at <= 0.0f && ab <= 0.0f) return;
        }
        glBegin(GL_QUADS);
        glColor4f(r, g, b, at);
        glTexCoord2f(cd->x0/fw, cd->y0/fh); glVertex2f(x0, y0);
        glTexCoord2f(cd->x1/fw, cd->y0/fh); glVertex2f(x1, y0);
        glColor4f(r, g, b, ab);
        glTexCoord2f(cd->x1/fw, cd->y1/fh); glVertex2f(x1, y1);
        glTexCoord2f(cd->x0/fw, cd->y1/fh); glVertex2f(x0, y1);
        glEnd();
    };

    auto emit_str = [&](float dx, float dy, float r, float g, float b, float a) {
        const char *p = text;
        float px = x + dx;
        float py = y + dy;
        unsigned int cp;
        while ((cp = utf8_next(&p)) != 0) {
            if (cp < 32 || cp > 255) { px += 8.0f * scale; continue; }
            stbtt_bakedchar *cd = &bc[cp - 32];
            if (cd->x0 == cd->x1) { px += cd->xadvance * scale; continue; }
            emit_glyph(px, py, cd, r, g, b, a);
            emit_glyph(px + 1.0f * scale, py, cd, r, g, b, a);
            px += cd->xadvance * scale;
        }
    };

    if (shadow)
        emit_str(shadow_dx, shadow_dy, 0, 0, 0, shadow_alpha * text_alpha);
    if (outline_px > 0.0f) {
        float o = outline_px;
        const float oa = 0.5f * text_alpha;
        emit_str(-o, -o, 0,0,0,oa); emit_str( 0,-o, 0,0,0,oa); emit_str( o,-o, 0,0,0,oa);
        emit_str(-o,  0, 0,0,0,oa);                             emit_str( o,  0, 0,0,0,oa);
        emit_str(-o,  o, 0,0,0,oa); emit_str( 0,  o, 0,0,0,oa); emit_str( o,  o, 0,0,0,oa);
    }
    emit_str(0, 0, cr, cg, cb, text_alpha);

    float px = x;
    const char *p = text;
    unsigned int cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp < 32 || cp > 255) { px += 8.0f * scale; continue; }
        px += bc[cp - 32].xadvance * scale;
    }
    return px + 1.0f * scale;
}

/* Simple word wrap into vector of lines. */
static std::vector<std::string> wrap_text(AchFont &font, const char *text,
                                          float max_w, float win_w, float win_h,
                                          float font_mul) {
    std::vector<std::string> lines;
    if (!text || !text[0]) return lines;
    auto width_of = [&](const std::string &s) {
        return measure_string(font, s.c_str(), false, win_w, win_h, font_mul);
    };
    std::string cur;
    std::string word;
    const char *p = text;
    while (true) {
        unsigned int cp = utf8_next(&p);
        if (cp == ' ' || cp == 0) {
            if (!word.empty()) {
                std::string trial = cur.empty() ? word : cur + " " + word;
                if (width_of(trial) > max_w && !cur.empty()) {
                    lines.push_back(cur);
                    cur = word;
                } else {
                    cur = trial;
                }
                word.clear();
            }
            if (cp == 0) break;
        } else {
            /* append utf-8 bytes for this codepoint */
            if (cp < 0x80) { word += (char)cp; }
            else if (cp < 0x800) { word += (char)(0xC0|(cp>>6)); word += (char)(0x80|(cp&0x3F)); }
            else { word += (char)(0xE0|(cp>>12)); word += (char)(0x80|((cp>>6)&0x3F)); word += (char)(0x80|(cp&0x3F)); }
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) lines.push_back(text);
    return lines;
}

/* =========================================================================
 * Globals
 * ========================================================================= */
static bool g_loaded = false;
static AchFont g_font;

/* Chrome textures */
static Tex g_tex_popup;
static Tex g_tex_menu;
static Tex g_tex_item_bg;
static Tex g_tex_medal;
static Tex g_tex_locked;

/* Per-achievement icon textures, lazy-loaded. -1 = not attempted, 0 = failed. */
static Tex g_icon_cache[kAchievementCount];
static bool g_icon_attempted[kAchievementCount];

static Tex &get_icon(int cat_idx) {
    if (cat_idx < 0 || cat_idx >= kAchievementCount) {
        static Tex dummy;
        return dummy;
    }
    if (!g_icon_attempted[cat_idx]) {
        g_icon_attempted[cat_idx] = true;
        g_icon_cache[cat_idx] = load_bmp_from_rc(kAchievements[cat_idx].icon_rc_id);
    }
    return g_icon_cache[cat_idx];
}

/* =========================================================================
 * Resource loading
 * ========================================================================= */
static void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;

    g_tex_popup = load_bmp_from_rc(kRcAchievementPopup);
    g_tex_menu  = load_bmp_from_rc(kRcAchievementMenu);
    g_tex_item_bg = load_bmp_from_rc(kRcAchievementItem);
    g_tex_medal = load_bmp_from_rc(kRcAchievementMedal);
    g_tex_locked = load_bmp_from_rc(kRcAchievementLocked);

    std::memset(g_icon_attempted, 0, sizeof(g_icon_attempted));

    g_font.load(213, 92.0f, 48.0f);  /* RC_CFG_FONT, header=92px, item=48px */
}

/* =========================================================================
 * Popup animation
 * ========================================================================= */
struct PopupState {
    int cat_idx = -1;         /* catalog index of the achievement */
    Uint32 start_ms = 0;
    bool active = false;
};

static PopupState g_popup;
static std::vector<int> g_popup_queue;  /* queued catalog indices */

static const float kPopupSlideUpMs   = 750.0f;
static const float kPopupHoldMs      = 2500.0f;
static const float kPopupSlideDownMs = 250.0f;
static const float kPopupTotalMs = kPopupSlideUpMs + kPopupHoldMs + kPopupSlideDownMs;

static void popup_start(int cat_idx) {
    g_popup.cat_idx = cat_idx;
    g_popup.start_ms = SDL_GetTicks();
    g_popup.active = true;
    config_screen_play_achievement_unlock_sound();
}

static void popup_advance() {
    if (!g_popup_queue.empty()) {
        popup_start(g_popup_queue.front());
        g_popup_queue.erase(g_popup_queue.begin());
    }
}

static void popup_enqueue(int cat_idx) {
    const pvz2_config_t *cfg = pvz2_config();
    if (!cfg || !cfg->achievement_notifications) return;
    if (g_popup.active) {
        g_popup_queue.push_back(cat_idx);
    } else {
        popup_start(cat_idx);
    }
}

/* Returns 0..1 for popup Y offset: 0 = fully visible, 1 = fully off-screen below. */
static float popup_offset_t() {
    if (!g_popup.active) return 1.0f;
    float elapsed = (float)(SDL_GetTicks() - g_popup.start_ms);
    if (elapsed < kPopupSlideUpMs) {
        return 1.0f - elapsed / kPopupSlideUpMs;
    } else if (elapsed < kPopupSlideUpMs + kPopupHoldMs) {
        return 0.0f;
    } else if (elapsed < kPopupTotalMs) {
        return (elapsed - kPopupSlideUpMs - kPopupHoldMs) / kPopupSlideDownMs;
    } else {
        g_popup.active = false;
        popup_advance();
        return 1.0f;
    }
}

/* =========================================================================
 * Screen state
 * ========================================================================= */
static bool g_screen_open = false;
static float g_screen_scroll = 0.0f;
static bool g_screen_dragging = false;
static int g_screen_drag_y = 0;
static float g_screen_drag_scroll_start = 0.0f;

/* Sorted order of items: completed first, then incomplete, preserving catalog order. */
struct ScreenItem {
    int cat_idx;
    bool completed;
};
static std::vector<ScreenItem> g_screen_items;

static void rebuild_screen_items() {
    std::lock_guard<std::mutex> lk(g_store_mtx);
    store_load_locked();
    g_screen_items.clear();
    /* First pass: completed */
    for (int i = 0; i < kAchievementCount; ++i) {
        bool done = false;
        for (int j = 0; j < kAchievementServiceMapCount; ++j) {
            if (std::strcmp(kAchievementServiceMap[j].catalog_id, kAchievements[i].id) == 0 &&
                g_completed.count(kAchievementServiceMap[j].service_id) != 0) {
                done = true;
                break;
            }
        }
        /* Also check if catalog id itself is in completed (self-mapped entries) */
        if (!done && g_completed.count(kAchievements[i].id) != 0) done = true;
        if (done) g_screen_items.push_back({i, true});
    }
    /* Second pass: incomplete */
    for (int i = 0; i < kAchievementCount; ++i) {
        bool found = false;
        for (const auto &si : g_screen_items)
            if (si.cat_idx == i) { found = true; break; }
        if (!found) g_screen_items.push_back({i, false});
    }
}

static int count_completed() {
    int n = 0;
    for (const auto &si : g_screen_items) if (si.completed) ++n;
    return n;
}

/* =========================================================================
 * GL draw helpers
 * ========================================================================= */
static void draw_quad(float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1,
                      float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x,     y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x,     y + h);
    glEnd();
}

static void draw_tex(const Tex &t, float x, float y, float w, float h, float a = 1.0f) {
    if (!t.id) return;
    glBindTexture(GL_TEXTURE_2D, t.id);
    draw_quad(x, y, w, h, 0, 0, 1, 1, 1, 1, 1, a);
}

/* Textured quad with different alpha on the top and bottom edges. */
static void draw_tex_vfade(const Tex &t, float x, float y, float w, float h,
                           float u0, float v0, float u1, float v1,
                           float a_top, float a_bot,
                           float cr = 1.0f, float cg = 1.0f, float cb = 1.0f) {
    if (!t.id || h <= 0.0f || w <= 0.0f) return;
    if (a_top <= 0.0f && a_bot <= 0.0f) return;
    glBindTexture(GL_TEXTURE_2D, t.id);
    glBegin(GL_QUADS);
    glColor4f(cr, cg, cb, a_top);
    glTexCoord2f(u0, v0); glVertex2f(x,     y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glColor4f(cr, cg, cb, a_bot);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x,     y + h);
    glEnd();
}

/* Draw a texture with vertical edge fade into the list (blend near top/bottom
 * of the list region; middle stays fully opaque). */
static void draw_tex_list_faded(const Tex &t, float x, float y, float w, float h,
                                float list_top, float list_bot,
                                float solid, float fade_end, float mul = 1.0f,
                                float cr = 1.0f, float cg = 1.0f, float cb = 1.0f) {
    if (!t.id || h <= 0.0f || w <= 0.0f || mul <= 0.0f) return;
    const float clip_top = list_top + solid;
    const float clip_bot = list_bot - solid;
    float y0 = y;
    float y1 = y + h;
    if (y1 <= clip_top || y0 >= clip_bot) return;
    if (y0 < clip_top) y0 = clip_top;
    if (y1 > clip_bot) y1 = clip_bot;
    if (y1 <= y0) return;

    const float fade_top_end = list_top + fade_end;
    const float fade_bot_start = list_bot - fade_end;

    auto emit_span = [&](float ya, float yb) {
        if (yb <= ya) return;
        const float v0 = (ya - y) / h;
        const float v1 = (yb - y) / h;
        float at = list_edge_alpha(ya, list_top, list_bot, solid, fade_end) * mul;
        float ab = list_edge_alpha(yb, list_top, list_bot, solid, fade_end) * mul;
        draw_tex_vfade(t, x, ya, w, yb - ya, 0, v0, 1, v1, at, ab, cr, cg, cb);
    };

    float cursor = y0;
    auto advance_to = [&](float limit) {
        if (cursor >= y1) return;
        float end = y1 < limit ? y1 : limit;
        if (end > cursor) {
            emit_span(cursor, end);
            cursor = end;
        }
    };
    advance_to(fade_top_end);
    advance_to(fade_bot_start);
    advance_to(y1);
}

/* =========================================================================
 * Popup draw
 * ========================================================================= */
static void draw_popup(SDL_Window *window) {
    if (!g_popup.active) return;

    float t = popup_offset_t();
    if (t >= 1.0f) return;  /* off-screen */

    int ww, wh;
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    float win_w = (float)ww, win_h = (float)wh;
    float s = win_h / kRefH;
    g_ui_scale = s;

    ensure_loaded();
    if (!g_tex_popup.id) return;

    float pw = sc((float)g_tex_popup.w, s);
    float ph = sc((float)g_tex_popup.h, s);

    float margin_bottom = sc(10.0f, s);
    /* Image already has left padding meant for centered placement; pull left. */
    float popup_x = sc(-56.0f, s);
    float popup_visible_y = win_h - ph - margin_bottom;
    float popup_y = popup_visible_y + t * (ph + margin_bottom);

    int cat_idx = g_popup.cat_idx;
    /* Icon below the popup bg (bg has a circular hole). Offset +2px from top. */
    if (cat_idx >= 0 && cat_idx < kAchievementCount) {
        Tex &icon = get_icon(cat_idx);
        if (icon.id) {
            float icon_w = sc((float)icon.w, s) * 0.9f;
            float icon_h = sc((float)icon.h, s) * 0.9f;
            float icon_x = popup_x + (pw - icon_w) * 0.5f;
            float icon_y = popup_y + sc(2.0f, s);
            draw_tex(icon, icon_x, icon_y, icon_w, icon_h);
        }
    }

    /* Popup background drawn on top of the icon */
    draw_tex(g_tex_popup, popup_x, popup_y, pw, ph);

    if (cat_idx >= 0 && cat_idx < kAchievementCount) {
        AchievementLang lang = current_lang();
        const AchievementDef &def = kAchievements[cat_idx];

        /* Title: 48pt (item bake, font_mul 1), yellow, baseline popup_top+186*s */
        if (g_font.ok) {
            float tx = popup_x + sc(105.0f, s);
            float ty = popup_y + sc(186.0f, s);
            draw_string_ach(g_font, def.title[lang], false, tx, ty,
                        0xfd/255.f, 0xdc/255.f, 0x67/255.f,
                        win_w, win_h, true, sc(3,s), sc(3,s), sc(1.5f,s), 0.25f, 1.0f);
        }

        /* Description: 30pt (item bake, font_mul 30/48), white, baseline popup_top+224*s */
        if (g_font.ok) {
            float desc_font_mul = 30.0f / 48.0f;
            float tx = popup_x + sc(105.0f, s);
            float max_w = pw - sc(105.0f, s) - sc(200.0f, s);
            std::vector<std::string> lines = wrap_text(g_font, def.desc[lang], max_w, win_w, win_h, desc_font_mul);
            float line_h = sc(30.0f, s);
            for (int li = 0; li < (int)lines.size() && li < 3; ++li) {
                float ty = popup_y + sc(224.0f, s) + (float)li * line_h;
                draw_string_ach(g_font, lines[li].c_str(), false, tx, ty,
                            1.0f, 1.0f, 1.0f,
                            win_w, win_h, true, sc(2,s), sc(2,s), 0, 0.3f, 1.0f, desc_font_mul);
            }
        }
    }
}

/* =========================================================================
 * Screen draw
 * ========================================================================= */
static void draw_screen(SDL_Window *window) {
    if (!g_screen_open) return;

    int ww, wh;
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    float win_w = (float)ww, win_h = (float)wh;
    float s = menu_scale(win_w, win_h);
    g_ui_scale = s;

    ensure_loaded();

    /* Darken full screen 50% */
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0, 0, 0, 0.5f);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(win_w, 0);
    glVertex2f(win_w, win_h); glVertex2f(0, win_h);
    glEnd();
    glEnable(GL_TEXTURE_2D);

    if (!g_tex_menu.id) return;

    float mw = sc((float)g_tex_menu.w, s);
    float mh = sc((float)g_tex_menu.h, s);
    float mx = (win_w - mw) * 0.5f;
    float my = (win_h - mh) * 0.5f;

    /* Shadow: 2px offset downward */
    float shadow_off = sc(2.0f, s);
    {
        glBindTexture(GL_TEXTURE_2D, g_tex_menu.id);
        glColor4f(0, 0, 0, 0.35f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(mx, my + shadow_off);
        glTexCoord2f(1, 0); glVertex2f(mx + mw, my + shadow_off);
        glTexCoord2f(1, 1); glVertex2f(mx + mw, my + shadow_off + mh);
        glTexCoord2f(0, 1); glVertex2f(mx, my + shadow_off + mh);
        glEnd();
    }
    /* Main menu panel */
    draw_tex(g_tex_menu, mx, my, mw, mh);

    AchievementLang lang = current_lang();

    /* Title: "Achievements" localized, yellow 92pt, centered X */
    if (g_font.ok) {
        const char *title = screen_title_for_lang(lang);
        float tw = measure_string(g_font, title, true, win_w, win_h);
        float tx = mx + (mw - tw) * 0.5f;
        float ty = my + sc(126.0f, s); /* was 150; raised 24px */
        draw_string_ach(g_font, title, true, tx, ty,
                    0xfd/255.f, 0xdc/255.f, 0x67/255.f,
                    win_w, win_h, true, sc(5,s), sc(5,s), sc(2,s));
    }

    /* Counter "N/75" white 60pt (header * 60/92), centered X */
    if (g_font.ok) {
        int completed = count_completed();
        char counter[32];
        SDL_snprintf(counter, sizeof(counter), "%d/%d", completed, kAchievementCount);
        float fmul = 60.0f / 92.0f;
        float tw = measure_string(g_font, counter, true, win_w, win_h, fmul);
        float tx = mx + (mw - tw) * 0.5f;
        float ty = my + sc(320.0f, s);
        draw_string_ach(g_font, counter, true, tx, ty,
                    1.0f, 1.0f, 1.0f,
                    win_w, win_h, true, sc(3,s)*fmul, sc(3,s)*fmul, 0, 0.3f, 1.0f, fmul);
    }

    /* List region in 1080p screen coords:
     * menu_x = (1920-1415)/2 = 252.5, menu_y = (1080-1047)/2 = 16.5
     * list screen: L=440, T=360, R=1480, B=1014
     * Convert to offsets from menu:
     *   list_off_x = 440 - 252.5 = 187.5
     *   list_off_y = 360 - 16.5  = 343.5
     *   list_w_ref = 1480 - 440  = 1040
     *   list_h_ref = 1014 - 360  = 654
     */
    float list_off_x = 187.5f;
    float list_off_y = 343.5f;
    float list_w_ref = 1040.0f;
    float list_h_ref = 654.0f;

    float lx = mx + sc(list_off_x, s);
    float ly = my + sc(list_off_y, s);
    float lw = sc(list_w_ref, s);
    float lh = sc(list_h_ref, s);

    /* 20px inset: first item starts here; fade only in the outer 20px bands. */
    const float list_pad = sc(20.0f, s);
    const float item_h = sc(161.0f, s);
    const float item_step = sc(181.0f, s); /* 161 item + 20 gap */
    int total_items = (int)g_screen_items.size();
    /* Top pad + items (last without trailing gap) + bottom pad. */
    float content_h = list_pad;
    if (total_items > 0)
        content_h += (float)(total_items - 1) * item_step + item_h + list_pad;
    float max_scroll = content_h - lh;
    if (max_scroll < 0) max_scroll = 0;
    if (g_screen_scroll < 0) g_screen_scroll = 0;
    if (g_screen_scroll > max_scroll) g_screen_scroll = max_scroll;

    /* Scissor to list region */
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)lx, (GLint)(win_h - ly - lh), (GLsizei)lw, (GLsizei)lh);

    float solid = 0.0f;                 /* fade starts at the rim */
    float fade_end = list_pad;          /* fully opaque after 20px */
    const float list_bot = ly + lh;

    /* Draw items — edge fade only in the first/last 20px of the list. */
    for (int idx = 0; idx < total_items; ++idx) {
        float iy = ly + list_pad + (float)idx * item_step - g_screen_scroll;
        float item_bottom = iy + item_h;
        if (item_bottom < ly) continue;
        if (iy > list_bot) break;

        const ScreenItem &si = g_screen_items[idx];
        int cat = si.cat_idx;
        const AchievementDef &def = kAchievements[cat];

        float item_left = lx + (lw - sc((float)g_tex_item_bg.w, s)) * 0.5f;
        float item_w = sc((float)g_tex_item_bg.w, s);

        /* Item background */
        if (g_tex_item_bg.id) {
            float bw = sc((float)g_tex_item_bg.w, s);
            float bh = sc((float)g_tex_item_bg.h, s);
            float bx = lx + (lw - bw) * 0.5f;
            draw_tex_list_faded(g_tex_item_bg, bx, iy, bw, bh, ly, list_bot, solid, fade_end);
        }

        /* Icon or locked icon at 50% size, left-aligned, centered Y (+1px down) */
        if (si.completed) {
            Tex &icon = get_icon(cat);
            if (icon.id) {
                float iw = sc((float)icon.w, s) * 0.5f;
                float ih = sc((float)icon.h, s) * 0.5f;
                float ix = item_left;
                float iy2 = iy + (item_h - ih) * 0.5f + sc(1.0f, s);
                draw_tex_list_faded(icon, ix, iy2, iw, ih, ly, list_bot, solid, fade_end);
            }
        } else if (g_tex_locked.id) {
            Tex &icon_ref = get_icon(cat);
            float ref_w = icon_ref.id ? (float)icon_ref.w : (float)g_tex_locked.w;
            float ref_h = icon_ref.id ? (float)icon_ref.h : (float)g_tex_locked.h;
            float iw = sc(ref_w, s) * 0.5f;
            float ih = sc(ref_h, s) * 0.5f;
            float ix = item_left;
            float iy2 = iy + (item_h - ih) * 0.5f + sc(1.0f, s);
            draw_tex_list_faded(g_tex_locked, ix, iy2, iw, ih, ly, list_bot, solid, fade_end);
        }

        float text_left = item_left + sc(170.0f, s);

        float medal_w = 0.0f, medal_h = 0.0f, medal_x = 0.0f;
        if (si.completed && g_tex_medal.id) {
            medal_w = sc((float)g_tex_medal.w, s);
            medal_h = sc((float)g_tex_medal.h, s);
            medal_x = item_left + item_w - sc(26.0f, s) - medal_w;
        }

        float text_right = si.completed && g_tex_medal.id
            ? (medal_x - sc(12.0f, s))
            : (item_left + item_w - sc(20.0f, s));
        float max_text_w = text_right - text_left;
        if (max_text_w < sc(40.0f, s)) max_text_w = sc(40.0f, s);

        /* Title: yellow 48pt */
        if (g_font.ok) {
            float ty = iy + sc(66.0f, s);
            std::vector<std::string> title_lines =
                wrap_text(g_font, def.title[lang], max_text_w, win_w, win_h, 1.0f);
            if (title_lines.empty()) title_lines.push_back(def.title[lang] ? def.title[lang] : "");
            float title_line_h = sc(40.0f, s);
            for (int li = 0; li < (int)title_lines.size() && li < 2; ++li) {
                draw_string_ach(g_font, title_lines[li].c_str(), false, text_left,
                            ty + (float)li * title_line_h,
                            0xfd/255.f, 0xdc/255.f, 0x67/255.f,
                            win_w, win_h, true, sc(2,s), sc(2,s), sc(1,s), 0.25f, 1.0f, 1.0f,
                            ly, list_bot, solid, fade_end);
            }
        }

        /* Description: white 30pt */
        if (g_font.ok) {
            float desc_mul = 30.0f / 48.0f;
            std::vector<std::string> lines = wrap_text(g_font, def.desc[lang], max_text_w, win_w, win_h, desc_mul);
            float line_h = sc(24.0f, s);
            for (int li = 0; li < (int)lines.size() && li < 3; ++li) {
                float ty = iy + sc(106.0f, s) + (float)li * line_h;
                draw_string_ach(g_font, lines[li].c_str(), false, text_left, ty,
                            1.0f, 1.0f, 1.0f,
                            win_w, win_h, true, sc(1.5f,s)*desc_mul, sc(1.5f,s)*desc_mul,
                            0, 0.3f, 1.0f, desc_mul,
                            ly, list_bot, solid, fade_end);
            }
        }

        /* Medal */
        if (si.completed && g_tex_medal.id) {
            float medal_y = iy + (item_h - medal_h) * 0.5f;
            float so = sc(2.0f, s);
            draw_tex_list_faded(g_tex_medal, medal_x, medal_y + so, medal_w, medal_h,
                                ly, list_bot, solid, fade_end, 0.3f, 0, 0, 0);
            draw_tex_list_faded(g_tex_medal, medal_x, medal_y, medal_w, medal_h,
                                ly, list_bot, solid, fade_end);
        }
    }

    glDisable(GL_SCISSOR_TEST);
}

/* =========================================================================
 * Main draw entry
 * ========================================================================= */
static void draw_all(SDL_Window *window) {
    if (!window) return;

    int ww, wh;
    SDL_GL_GetDrawableSize(window, &ww, &wh);

    GLint prev_program = 0, prev_fbo = 0;
    GLint prev_viewport[4]{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, (double)ww, (double)wh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();

    draw_screen(window);
    draw_popup(window);

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    glPopAttrib();
    glUseProgram(prev_program);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

/* =========================================================================
 * Event handling for screen
 * ========================================================================= */
static void handle_screen_event(const SDL_Event *e) {
    if (!g_screen_open || !e) return;

    int ww_i, wh_i;
    /* Need a window to get drawable size; try SDL_GL_GetDrawableSize on the
       focused window. Use 1920x1080 as fallback. */
    SDL_Window *focus = SDL_GetMouseFocus();
    if (focus)
        SDL_GL_GetDrawableSize(focus, &ww_i, &wh_i);
    else { ww_i = 1920; wh_i = 1080; }
    float win_w = (float)ww_i, win_h = (float)wh_i;
    float s = menu_scale(win_w, win_h);

    float mw = sc((float)g_tex_menu.w, s);
    float mh = sc((float)g_tex_menu.h, s);
    float mx = (win_w - mw) * 0.5f;
    float my = (win_h - mh) * 0.5f;

    float list_off_x = 187.5f;
    float list_off_y = 343.5f;
    float list_w_ref = 1040.0f;
    float list_h_ref = 654.0f;
    float lx = mx + sc(list_off_x, s);
    float ly = my + sc(list_off_y, s);
    float lw = sc(list_w_ref, s);
    float lh = sc(list_h_ref, s);

    const float list_pad = sc(20.0f, s);
    const float item_h = sc(161.0f, s);
    const float item_step = sc(181.0f, s);
    auto max_scroll_for = [&]() {
        const int n = (int)g_screen_items.size();
        float content_h = list_pad;
        if (n > 0)
            content_h += (float)(n - 1) * item_step + item_h + list_pad;
        float ms = content_h - lh;
        return ms > 0.0f ? ms : 0.0f;
    };

    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        float rx = (float)e->button.x;
        float ry = (float)e->button.y;
        /* Click outside menu panel closes screen */
        if (rx < mx || rx > mx + mw || ry < my || ry > my + mh) {
            g_screen_open = false;
            return;
        }
        /* Start drag for scrolling within list region */
        if (rx >= lx && rx <= lx + lw && ry >= ly && ry <= ly + lh) {
            g_screen_dragging = true;
            g_screen_drag_y = (int)ry;
            g_screen_drag_scroll_start = g_screen_scroll;
        }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        g_screen_dragging = false;
    } else if (e->type == SDL_MOUSEMOTION && g_screen_dragging) {
        float dy = (float)(g_screen_drag_y - e->motion.y);
        g_screen_scroll = g_screen_drag_scroll_start + dy;
        float max_scroll = max_scroll_for();
        if (g_screen_scroll < 0) g_screen_scroll = 0;
        if (g_screen_scroll > max_scroll) g_screen_scroll = max_scroll;
    } else if (e->type == SDL_MOUSEWHEEL) {
        g_screen_scroll -= e->wheel.y * item_step * 0.5f;
        float max_scroll = max_scroll_for();
        if (g_screen_scroll < 0) g_screen_scroll = 0;
        if (g_screen_scroll > max_scroll) g_screen_scroll = max_scroll;
    }
}

/* =========================================================================
 * Public C API implementation
 * ========================================================================= */
extern "C" {

int achievements_notify_service_unlocked(const char *service_id) {
    if (!service_id) return 0;
    {
        std::lock_guard<std::mutex> lk(g_store_mtx);
        store_load_locked();
        if (g_completed.count(service_id) != 0) return 0;
        g_completed.insert(service_id);
        store_save_locked();
    }
    /* Map to catalog id and show popup */
    const char *cat_id = service_to_catalog(service_id);
    if (cat_id) {
        int idx = find_catalog_index(cat_id);
        if (idx >= 0) popup_enqueue(idx);
    }
    return 1;
}

void achievements_show_popup(const char *catalog_id) {
    int idx = find_catalog_index(catalog_id);
    if (idx >= 0) popup_enqueue(idx);
}

void achievements_screen_request_open(void) {
    if (g_screen_open) return;
    rebuild_screen_items();
    g_screen_scroll = 0;
    g_screen_dragging = false;
    g_screen_open = true;
    /* Same cue as opening the quit dialog. */
    config_screen_play_exit_sound();
}

void achievements_screen_request_close(void) {
    g_screen_open = false;
    g_screen_dragging = false;
}

int achievements_screen_is_open(void) {
    return g_screen_open ? 1 : 0;
}

int achievements_ui_is_active(void) {
    return (g_screen_open || g_popup.active) ? 1 : 0;
}

void achievements_screen_handle_event(const SDL_Event *e) {
    handle_screen_event(e);
}

void achievements_draw(SDL_Window *window) {
    /* Draw if screen is open OR popup is active */
    if (!g_screen_open && !g_popup.active) return;
    draw_all(window);
}

void achievements_shutdown(void) {
    g_font.unload();
    free_tex(g_tex_popup);
    free_tex(g_tex_menu);
    free_tex(g_tex_item_bg);
    free_tex(g_tex_medal);
    free_tex(g_tex_locked);
    for (int i = 0; i < kAchievementCount; ++i) free_tex(g_icon_cache[i]);
    std::memset(g_icon_attempted, 0, sizeof(g_icon_attempted));
    g_loaded = false;
    g_popup.active = false;
    g_popup_queue.clear();
    g_screen_open = false;
}

int achievements_is_service_completed(const char *service_id) {
    if (!service_id) return 0;
    std::lock_guard<std::mutex> lk(g_store_mtx);
    store_load_locked();
    return g_completed.count(service_id) != 0 ? 1 : 0;
}

void achievements_reset_completed(void) {
    std::lock_guard<std::mutex> lk(g_store_mtx);
    g_completed.clear();
    g_store_loaded = true;
    store_save_locked();
}

} /* extern "C" */
