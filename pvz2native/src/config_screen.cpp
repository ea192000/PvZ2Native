/* In-game settings overlay.
 *
 * Renders a configuration screen using OpenGL fixed-function (same context as
 * the game). Assets are embedded in the executable as RCDATA resources.
 * Text is rendered via stb_truetype into a per-frame glyph atlas texture.
 * Audio effects are played via a dedicated SDL audio device (does not touch
 * the game's audio device).
 *
 * Public API: see include/pvz2native/config_screen.h
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <pvz2native/config_screen.h>
#include <pvz2native/config.h>
#include <pvz2native/achievements.h>
#include <pvz2native/dex/config_store.h>
#include <pvz2native/gfx/frame_limiter.h>
#include <pvz2native/host_assets.h>
#include <pvz2native/host_paths.h>
#include <pvz2native/input/gamepad.h>

#include <SDL.h>
#include <glad/gl.h>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

/* =========================================================================
 * Resource IDs (must match windows_resources.rc)
 * ========================================================================= */
#define RC_CFG_PANEL_BG       200
#define RC_CFG_LIST_BG        201
#define RC_CFG_LIST_ITEM      202
#define RC_CFG_BTN_CLOSE_UP   203
#define RC_CFG_BTN_CLOSE_DOWN 204
#define RC_CFG_BTN_BACK_UP    205
#define RC_CFG_BTN_BACK_DOWN  206
#define RC_CFG_BTN_ARROW_UP   207
#define RC_CFG_BTN_ARROW_DOWN 208
#define RC_CFG_CHK_CHECKED    209
#define RC_CFG_CHK_UNCHECKED  210
#define RC_CFG_SCROLL_TRACK   211
#define RC_CFG_SCROLL_BAR     212
#define RC_CFG_FONT           213
#define RC_CFG_SND_CHECK      214
#define RC_CFG_SND_NEXT       215
#define RC_CFG_SND_CLOSE      216
#define RC_CFG_SND_BACK       217
#define RC_CFG_BTN_EDIT_UP    218
#define RC_CFG_BTN_EDIT_DOWN  219
#define RC_CFG_BTN_SAVE_UP    220
#define RC_CFG_BTN_SAVE_DOWN  221
#define RC_CFG_TXT_BIND_INPUT 222
#define RC_CFG_IMG_BIND_CONT  223
#define RC_CFG_IMG_BIND_PREV  224
#define RC_CFG_POINT_GREEN    225
#define RC_CFG_SND_CONFIRM    226
#define RC_CFG_BTN_EXIT_UP    227
#define RC_CFG_BTN_EXIT_DOWN  228
#define RC_CFG_BTN_CANCEL_UP  229
#define RC_CFG_BTN_CANCEL_DOWN 230
#define RC_CFG_PANEL_EXIT_BG  231
#define RC_CFG_SND_EXIT       232
#define RC_CFG_SLIDER_THUMB   233
#define RC_CFG_SLIDER_EMPTY   234
#define RC_CFG_SLIDER_FULL    235
#define RC_CFG_SND_SLIDER     236
#define RC_CFG_SND_ACHIEVE_UNLOCK 317

/* =========================================================================
 * Layout constants (all in reference pixels @ 1920x1080)
 * ========================================================================= */
static const float kRefW = 1920.0f;
static const float kRefH = 1080.0f;

/* Panel: centered in window */
/* list_bg offset inside panel */
static const float kListOffX   = 72.0f;   /* from panel left edge */
static const float kListOffY   = 143.0f;  /* from panel top edge  */

/* Scroll track inside list_bg */
static const float kScrollTrackOffX = 11.0f;  /* from list_bg right edge (scrollbar on the right) */
static const float kScrollTrackOffY = 14.0f;  /* from list_bg top  */

/* List items */
/* First/last item flush to the solid 6px rim; top 10px (6..16) fade under the shadow. */
static const float kItemStartY = 6.0f;  /* == kListSolidEdge: top of first item at rest */
static const float kListSolidEdge = 6.0f;
static const float kListFadeEnd   = 11.0f; /* solid + 5px fade → full opacity */
static const float kListCornerR   = 20.0f; /* list_bg rounded-corner radius (stencil) */
/* Diagonal chamfer size for list-item corners (from the solid rim inward).
 * Large enough to eat the square cream tip that sat inside list_bg's curve. */
static const float kListCornerCut  = 14.0f;

/* Item text left padding from item left edge */
static const float kItemTextOffX = 67.0f;

/* Button positions inside panel (from panel edges) */
static const float kCloseBtnFromTop   = 36.0f;
static const float kCloseBtnFromRight = 60.0f;
static const float kBackBtnFromTop    = 36.0f;
static const float kBackBtnFromLeft   = 72.0f;

/* Arrow / checkbox right margin from item right edge */
static const float kArrowFromRight = 43.0f;
static const float kChkFromRight   = 34.0f;
/* Slider track: right edge of track is this far from list_item right edge. */
static const float kSliderFromRight = 74.0f;
static const float kSliderThumbShadowPx = 4.5f;
static const float kSliderSpinDeg = 450.0f; /* 0%→100% thumb rotation */
static const float kStickSpeedMin = 0.20f;
static const float kStickSpeedMax = 3.00f;
static const float kStickDeadzoneMin = 0.00f;
static const float kStickDeadzoneMax = 0.50f;

/* Font sizes in atlas pixels at 72 ppi (pt == px at 72 ppi). */
static const float kHeaderFontPx   = 77.0f;  /* 72pt @ 72ppi */
static const float kItemFontPx     = 50.0f;  /* 47pt @ 72ppi */
static const float kHeaderBaselineY = 101.0f; /* from panel top, reference 1080p */

/* Quit-confirm panel layout (reference pixels @ 1080p, relative to panel top) */
/* Message sits vertically between the title (~101) and the buttons (485). */
static const float kQuitMsgY      = 280.0f;  /* first line baseline of body text */
static const float kQuitMsgLineH  = 58.0f;   /* line spacing for 2-line message */
static const float kQuitBtnY      = 485.0f;  /* top of QUIT / CANCEL buttons */
static const float kQuitBtnGap    = 40.0f;   /* horizontal gap between buttons */

/* =========================================================================
 * Localization
 * ========================================================================= */
struct Strings {
	const char *title;           /* "Settings" / "Ajustes" */
	const char *resolution;      /* option label */
	const char *fps;
	const char *language;
	const char *currency;
	const char *iap;
	const char *iap_on;
	const char *iap_off;
	const char *fullscreen;
	const char *fps_unlimited;   /* "Unlimited" / "Sin límite" */
	/* language names */
	const char *lang_en;
	const char *lang_es;
	const char *lang_fr;
	const char *lang_de;
	const char *lang_pt;
	const char *lang_it;
	/* currency names (human-readable) */
	const char *cur_usd;
	const char *cur_eur;
	const char *cur_gbp;
	const char *cur_brl;
	const char *cur_peso;
	const char *input_config; /* "Input Configuration" label */
	const char *gamepad_config; /* "Gamepad Configuration" label */
	const char *gamepad_selected; /* "Selected Gamepad" row */
	const char *gamepad_disabled; /* "Disabled" device option / status */
	const char *gamepad_stick_speed;
	const char *gamepad_stick_deadzone;
	const char *gamepad_none; /* "None" for unassigned pad bind */
	const char *gamepad_btn_b;
	const char *gamepad_btn_x;
	const char *gamepad_btn_y;
	const char *gamepad_btn_l3;
	const char *gamepad_btn_r3;
	const char *view_achievements; /* "View Achievements" menu row */
	const char *achievement_notify; /* checkbox: unlock popup + sound */
	const char *exit_program; /* same wording as quit_title (menu row) */
	const char *save;         /* "SAVE" / "GUARDAR" */
	/* Quit-confirm dialog */
	const char *quit_title;   /* "Quit Game" */
	const char *quit_msg1;    /* body line 1 */
	const char *quit_msg2;    /* body line 2 */
	const char *quit_btn;     /* "QUIT" */
	const char *cancel_btn;   /* "CANCEL" */
	const char *display_config; /* "Display" row label / picker screen title */
};

static const Strings kStringsEN = {
	"Configuration",
	"Resolution", "Refresh Rate", "Language", "Currency", "Free Purchases",
	"On", "Off",
	"Fullscreen",
	"Unlimited",
	"English","Spanish","French","German","Portuguese","Italian",
	"Dollar","Euro","Pound","Real","Peso",
	"Input Configuration",
	"Gamepad Configuration", "Selected Gamepad", "Disabled",
	"Stick Speed", "Deadzone", "None",
	"Button B", "Button X", "Button Y", "Button L3", "Button R3",
	"View Achievements",
	"Achievement Notifications",
	"Quit Game", "SAVE",
	"Quit Game",
	"Are you sure you wish to",
	"quit the game?",
	"QUIT", "CANCEL",
	"Display"
};
static const Strings kStringsES = {
	"Configuración",
	"Resolución", "Tasa de refresco", "Idioma", "Moneda", "Compras gratuitas",
	"Activado", "Desactivado",
	"Pantalla completa",
	"Sin límite",
	"Inglés","Español","Francés","Alemán","Portugués","Italiano",
	"Dólar","Euro","Libra","Real","Peso",
	"Configuración de entrada",
	"Configuración de mando", "Mando seleccionado", "Desactivado",
	"Velocidad", "Zona muerta", "Ninguno",
	"Botón B", "Botón X", "Botón Y", "Botón L3", "Botón R3",
	"Ver logros",
	"Notificaciones de logros",
	"Salir del juego", "GUARDAR",
	"Salir del juego",
	"¿Seguro que deseas",
	"salir del juego?",
	"SALIR", "CANCELAR",
	"Pantalla"
};
static const Strings kStringsFR = {
	"Configuration",
	"Résolution", "Fréquence d'affichage", "Langue", "Devise", "Achats gratuits",
	"Activé", "Désactivé",
	"Plein écran",
	"Illimité",
	"Anglais","Espagnol","Français","Allemand","Portugais","Italien",
	"Dollar","Euro","Livre","Réal","Peso",
	"Configuration des touches",
	"Configuration de la manette", "Manette sélectionnée", "Désactivée",
	"Vitesse", "Zone morte", "Aucun",
	"Bouton B", "Bouton X", "Bouton Y", "Bouton L3", "Bouton R3",
	"Voir les succ\xc3\xa8s",
	"Notifications de succ\xc3\xa8s",
	"Quitter le jeu", "ENREGISTRER",
	"Quitter le jeu",
	"Voulez-vous vraiment",
	"quitter le jeu ?",
	"QUITTER", "ANNULER",
	"Écran"
};
static const Strings kStringsDE = {
	"Konfiguration",
	"Auflösung", "Bildwiederholrate", "Sprache", "Währung", "Gratis-Käufe",
	"Ein", "Aus",
	"Vollbild",
	"Unbegrenzt",
	"Englisch","Spanisch","Französisch","Deutsch","Portugiesisch","Italienisch",
	"Dollar","Euro","Pfund","Real","Peso",
	"Eingabe-Konfiguration",
	"Controller-Konfiguration", "Ausgewählter Controller", "Deaktiviert",
	"Geschwindigkeit", "Totzone", "Keine",
	"Taste B", "Taste X", "Taste Y", "Taste L3", "Taste R3",
	"Erfolge ansehen",
	"Erfolgs-Benachrichtigungen",
	"Spiel beenden", "SPEICHERN",
	"Spiel beenden",
	"Möchten Sie das Spiel",
	"wirklich beenden?",
	"BEENDEN", "ABBRECHEN",
	"Bildschirm"
};
static const Strings kStringsPT = {
	"Configurações",
	"Resolução", "Taxa de atualização", "Idioma", "Moeda", "Compras gratuitas",
	"Ativado", "Desativado",
	"Tela cheia",
	"Ilimitado",
	"Inglês","Espanhol","Francês","Alemão","Português","Italiano",
	"Dólar","Euro","Libra","Real","Peso",
	"Configuração de entrada",
	"Configuração do controle", "Controle selecionado", "Desativado",
	"Velocidade", "Zona morta", "Nenhum",
	"Botão B", "Botão X", "Botão Y", "Botão L3", "Botão R3",
	"Ver conquistas",
	"Notificações de conquistas",
	"Sair do jogo", "SALVAR",
	"Sair do jogo",
	"Tem certeza de que deseja",
	"sair do jogo?",
	"SAIR", "CANCELAR",
	"Tela"
};
static const Strings kStringsIT = {
	"Configurazione",
	"Risoluzione", "Frequenza di aggiornamento", "Lingua", "Valuta", "Acquisti gratuiti",
	"Attivato", "Disattivato",
	"Schermo intero",
	"Illimitato",
	"Inglese","Spagnolo","Francese","Tedesco","Portoghese","Italiano",
	"Dollaro","Euro","Sterlina","Real","Peso",
	"Configurazione dei comandi",
	"Configurazione del controller", "Controller selezionato", "Disattivato",
	"Velocità", "Zona morta", "Nessuno",
	"Pulsante B", "Pulsante X", "Pulsante Y", "Pulsante L3", "Pulsante R3",
	"Vedi obiettivi",
	"Notifiche obiettivi",
	"Esci dal gioco", "SALVA",
	"Esci dal gioco",
	"Sei sicuro di voler",
	"uscire dal gioco?",
	"ESCI", "ANNULLA",
	"Schermo"
};

static const Strings *get_strings(const char *locale) {
	if (!locale || !locale[0]) return &kStringsEN;
	if (strncmp(locale, "es", 2) == 0) return &kStringsES;
	if (strncmp(locale, "fr", 2) == 0) return &kStringsFR;
	if (strncmp(locale, "de", 2) == 0) return &kStringsDE;
	if (strncmp(locale, "pt", 2) == 0) return &kStringsPT;
	if (strncmp(locale, "it", 2) == 0) return &kStringsIT;
	return &kStringsEN;
}

/* Known default bind_* labels. Custom binds fall back to Title Case of the id. */
struct BindLabel {
	const char *id;
	const char *en;
	const char *es;
	const char *fr;
	const char *de;
	const char *pt;
	const char *it;
};

static const BindLabel kBindLabels[] = {
	{"plant_1",     "Plant 1",      "Planta 1",       "Plante 1",        "Pflanze 1",         "Planta 1",       "Pianta 1"},
	{"plant_2",     "Plant 2",      "Planta 2",       "Plante 2",        "Pflanze 2",         "Planta 2",       "Pianta 2"},
	{"plant_3",     "Plant 3",      "Planta 3",       "Plante 3",        "Pflanze 3",         "Planta 3",       "Pianta 3"},
	{"plant_4",     "Plant 4",      "Planta 4",       "Plante 4",        "Pflanze 4",         "Planta 4",       "Pianta 4"},
	{"plant_5",     "Plant 5",      "Planta 5",       "Plante 5",        "Pflanze 5",         "Planta 5",       "Pianta 5"},
	{"plant_6",     "Plant 6",      "Planta 6",       "Plante 6",        "Pflanze 6",         "Planta 6",       "Pianta 6"},
	{"plant_7",     "Plant 7",      "Planta 7",       "Plante 7",        "Pflanze 7",         "Planta 7",       "Pianta 7"},
	{"plant_8",     "Plant 8",      "Planta 8",       "Plante 8",        "Pflanze 8",         "Planta 8",       "Pianta 8"},
	{"replay",      "Replay",       "Repetir",        "Rejouer",         "Wiederholen",       "Repetir",        "Rigioca"},
	{"speed",       "Speed",        "Velocidad",      "Vitesse",         "Geschwindigkeit",   "Velocidade",     "Velocità"},
	{"pause",       "Pause",        "Pausa",          "Pause",           "Pause",             "Pausa",          "Pausa"},
	{"plant_food",  "Plant Food",   "Nutriente",      "Engrais",         "Pflanzennahrung",   "Adubo",          "Ricostituente"},
	{"power_1",     "Power 1",      "Poder 1",        "Pouvoir 1",       "Power 1",           "Poder 1",        "Potere 1"},
	{"power_2",     "Power 2",      "Poder 2",        "Pouvoir 2",       "Power 2",           "Poder 2",        "Potere 2"},
	{"power_3",     "Power 3",      "Poder 3",        "Pouvoir 3",       "Power 3",           "Poder 3",        "Potere 3"},
	{"shovel",      "Shovel",       "Pala",           "Pelle",           "Schaufel",          "Pá",             "Pala"},
	{"back",        "Back",         "Atrás",          "Retour",          "Zurück",            "Voltar",         "Indietro"},
	{"almanaq",     "Almanac",      "Almanaque",      "Almanach",        "Almanach",          "Almanaque",      "Almanacco"},
	{"connect",     "Connect",      "Conectar",       "Connexion",       "Verbinden",         "Conectar",       "Connetti"},
	{"vasebreaker", "Vasebreaker",  "Rompejarrones",  "Casse-vases",     "Vasenbrecher",      "Quebra-vasos",   "Spaccavasi"},
	{"zen_garden",  "Zen Garden",   "Jardín Zen",     "Jardin Zen",      "Zen-Garten",        "Jardim Zen",     "Giardino Zen"},
};

/* Title-Case fallback: "zen_garden" → "Zen Garden" (existing behaviour). */
static std::string format_bind_name_fallback(const char *raw) {
	std::string display;
	if (!raw) return display;
	bool cap = true;
	for (const char *p = raw; *p; ++p) {
		char c = *p;
		if (c == '_' || c == '-') { display += ' '; cap = true; }
		else if (cap) { display += (char)std::toupper((unsigned char)c); cap = false; }
		else { display += c; }
	}
	return display;
}

static const char *lookup_bind_label(const char *name, const char *locale) {
	if (!name || !name[0]) return nullptr;
	for (const BindLabel &bl : kBindLabels) {
		if (strcmp(bl.id, name) != 0) continue;
		if (locale && strncmp(locale, "es", 2) == 0) return bl.es;
		if (locale && strncmp(locale, "fr", 2) == 0) return bl.fr;
		if (locale && strncmp(locale, "de", 2) == 0) return bl.de;
		if (locale && strncmp(locale, "pt", 2) == 0) return bl.pt;
		if (locale && strncmp(locale, "it", 2) == 0) return bl.it;
		return bl.en;
	}
	return nullptr;
}

static std::string bind_display_name(const char *name, const char *locale) {
	if (const char *tr = lookup_bind_label(name, locale))
		return std::string(tr);
	return format_bind_name_fallback(name);
}

/* =========================================================================
 * Texture helpers
 * ========================================================================= */
struct Tex {
	GLuint id = 0;
	int w = 0, h = 0;
};

extern "C" void apply_magenta_colorkey(SDL_Surface *s) {
	if (!s || s->format->format != SDL_PIXELFORMAT_RGBA32) return;
	const int total = s->w * s->h;
	Uint8 *px = (Uint8 *)s->pixels;
	for (int i = 0; i < total; ++i) {
		Uint8 *p = px + i * 4;
		/* Magenta key (#FF00FF) + anti-aliased fringe → fully transparent */
		if (p[0] >= 200 && p[1] <= 60 && p[2] >= 200) {
			p[0] = p[1] = p[2] = 0;
			p[3] = 0;
		}
	}
}

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
	if (!file_buf || bytes <= 0) {
		if (file_buf) SDL_free(file_buf);
		return t;
	}
	rw = SDL_RWFromConstMem(file_buf, bytes);
#endif
	if (!rw) {
		if (file_buf) SDL_free(file_buf);
		return t;
	}
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

/* Load raw bytes from RC (Windows) or <exe>/assets/ (elsewhere).
 * Caller owns and must SDL_free(). */
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

/* =========================================================================
 * Font rendering via stb_truetype
 * ========================================================================= */
struct Font {
	stbtt_fontinfo info{};
	void *data = nullptr;         /* font file bytes, owned */
	GLuint atlas_tex = 0;
	static const int kAtlasW = 2048;
	static const int kAtlasH = 2048;
	unsigned char atlas_buf[kAtlasW * kAtlasH]{};
	stbtt_bakedchar glyphs_header[224]{};  /* ASCII 32..255 baked at header size */
	stbtt_bakedchar glyphs_item[224]{};    /* ASCII 32..255 baked at item size   */
	float scale_header = 0.0f;
	float scale_item   = 0.0f;
	int   ascent_unscaled = 0; /* raw font units, use scale_* to convert */
	bool ok = false;

	/* Bake two sizes into the same atlas (upper/lower halves).
	 * px_header and px_item are pixel heights at the REFERENCE resolution
	 * (kRefH = 1080). They are fixed values, not DPI-scaled, so they always
	 * fit in the 2048×1024 half-atlas. */
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

		/* Bake header glyphs in top half */
		stbtt_BakeFontBitmap((unsigned char *)data, 0, px_header,
							 atlas_buf, kAtlasW, kAtlasH / 2, 32, 224, glyphs_header);
		/* Bake item glyphs in bottom half — offset in the buffer */
		stbtt_BakeFontBitmap((unsigned char *)data, 0, px_item,
							 atlas_buf + kAtlasW * (kAtlasH / 2), kAtlasW, kAtlasH / 2,
							 32, 224, glyphs_item);
		/* Adjust y0/y1 in item glyphs to point into the full atlas */
		for (int i = 0; i < 224; ++i) {
			glyphs_item[i].y0 += (short)(kAtlasH / 2);
			glyphs_item[i].y1 += (short)(kAtlasH / 2);
		}

		/* Upload as GL_ALPHA: fixed-function GL_MODULATE multiplies vertex
		 * color by texture alpha, giving correct glyph coverage blending. */
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
 * GL draw helpers (fixed-function, same as splash screen)
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

/* Draw a horizontal crop of the texture (u0..u1 in 0..1) into a dest rect. */
static void draw_tex_hcrop(const Tex &t, float x, float y, float w, float h,
						   float u0, float u1, float a = 1.0f) {
	if (!t.id || w <= 0.0f || h <= 0.0f || u1 <= u0) return;
	glBindTexture(GL_TEXTURE_2D, t.id);
	draw_quad(x, y, w, h, u0, 0, u1, 1, 1, 1, 1, a);
}

/* Rotated textured quad around (cx,cy).
 * deg > 0 spins the thumb clockwise when the slider moves right (t increases). */
static void draw_tex_rotated(const Tex &t, float cx, float cy, float w, float h,
							 float deg, float r, float g, float b, float a) {
	if (!t.id || a <= 0.0f) return;
	const float rad = deg * 3.14159265f / 180.0f;
	const float c = std::cos(rad);
	const float sn = std::sin(rad);
	const float hx = w * 0.5f;
	const float hy = h * 0.5f;
	const float ox[4] = {-hx, hx, hx, -hx};
	const float oy[4] = {-hy, -hy, hy, hy};
	const float uu[4] = {0, 1, 1, 0};
	const float vv[4] = {0, 0, 1, 1};
	glBindTexture(GL_TEXTURE_2D, t.id);
	glColor4f(r, g, b, a);
	glBegin(GL_QUADS);
	for (int i = 0; i < 4; ++i) {
		/* Screen-space (Y-down) clockwise for +deg:
		 *   x' =  x cos(θ) - y sin(θ)
		 *   y' =  x sin(θ) + y cos(θ)
		 * (opposite of the previous matrix, which spun the wrong way). */
		float px = cx + ox[i] * c - oy[i] * sn;
		float py = cy + ox[i] * sn + oy[i] * c;
		glTexCoord2f(uu[i], vv[i]);
		glVertex2f(px, py);
	}
	glEnd();
}

/* Alpha for a Y position inside list_bg: 0 in the solid rim, ramp through the
 * shadow band, 1 in the flat content area. */
static float list_edge_alpha(float y, float list_top, float list_bot,
							 float solid, float fade_end) {
	const float span = fade_end - solid;
	if (span <= 0.0f) return 1.0f;
	float a = 1.0f;
	const float d_top = y - list_top;
	if (d_top < fade_end) {
		if (d_top <= solid) return 0.0f;
		a = (d_top - solid) / span;
	}
	const float d_bot = list_bot - y;
	if (d_bot < fade_end) {
		if (d_bot <= solid) return 0.0f;
		const float ab = (d_bot - solid) / span;
		if (ab < a) a = ab;
	}
	return a;
}

/* Diagonal corner chamfer inset, absolute in list space.
 *
 * White "puntas" = square cream corners of list_item drawn over list_bg's
 * curved shadow. A circular clip anchored at the outer edge barely cut
 * anything once past the 6px solid rim. So we chamfer from the solid rim:
 *   inset = cut - depth   (depth 0 at solid → inset=cut; depth=cut → 0)
 * One straight 45° cut per corner, same for every row at that Y. */
static float list_corner_inset(float y, float list_top, float list_bot,
							   float cut, float solid) {
	if (cut <= 0.0f) return 0.0f;
	float inset = 0.0f;
	float d = y - (list_top + solid);
	if (d < cut) {
		float v = (d <= 0.0f) ? cut : (cut - d);
		if (v > inset) inset = v;
	}
	d = (list_bot - solid) - y;
	if (d < cut) {
		float v = (d <= 0.0f) ? cut : (cut - d);
		if (v > inset) inset = v;
	}
	return inset;
}

/* Draw a textured quad with different alpha at the top and bottom edges. */
static void draw_tex_vfade(const Tex &t, float x, float y, float w, float h,
						   float u0, float v0, float u1, float v1,
						   float a_top, float a_bot) {
	if (!t.id || h <= 0.0f || w <= 0.0f) return;
	if (a_top <= 0.0f && a_bot <= 0.0f) return;
	glBindTexture(GL_TEXTURE_2D, t.id);
	glBegin(GL_QUADS);
	glColor4f(1, 1, 1, a_top);
	glTexCoord2f(u0, v0); glVertex2f(x,     y);
	glTexCoord2f(u1, v0); glVertex2f(x + w, y);
	glColor4f(1, 1, 1, a_bot);
	glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
	glTexCoord2f(u0, v1); glVertex2f(x,     y + h);
	glEnd();
}

static void draw_tex_vfade_trap(const Tex &t,
								float x0t, float x1t, float yt,
								float x0b, float x1b, float yb,
								float u0t, float u1t, float vt,
								float u0b, float u1b, float vb,
								float a_top, float a_bot) {
	if (!t.id || yb <= yt) return;
	if (x1t <= x0t && x1b <= x0b) return;
	if (a_top <= 0.0f && a_bot <= 0.0f) return;
	if (x1t <= x0t) { x0t = x0b; x1t = x1b; u0t = u0b; u1t = u1b; }
	if (x1b <= x0b) { x0b = x0t; x1b = x1t; u0b = u0t; u1b = u1t; }
	glBindTexture(GL_TEXTURE_2D, t.id);
	glBegin(GL_QUADS);
	glColor4f(1, 1, 1, a_top);
	glTexCoord2f(u0t, vt); glVertex2f(x0t, yt);
	glTexCoord2f(u1t, vt); glVertex2f(x1t, yt);
	glColor4f(1, 1, 1, a_bot);
	glTexCoord2f(u1b, vb); glVertex2f(x1b, yb);
	glTexCoord2f(u0b, vb); glVertex2f(x0b, yb);
	glEnd();
}

/* Trapezoid with per-corner alpha — used to feather the diagonal chamfer. */
static void draw_tex_vfade_trap4(const Tex &t,
								 float x0t, float x1t, float yt,
								 float x0b, float x1b, float yb,
								 float u0t, float u1t, float vt,
								 float u0b, float u1b, float vb,
								 float a_tl, float a_tr, float a_br, float a_bl) {
	if (!t.id || yb <= yt) return;
	if (x1t <= x0t && x1b <= x0b) return;
	if (a_tl <= 0.0f && a_tr <= 0.0f && a_br <= 0.0f && a_bl <= 0.0f) return;
	if (x1t <= x0t) { x0t = x0b; x1t = x1b; u0t = u0b; u1t = u1b; a_tl = a_bl; a_tr = a_br; }
	if (x1b <= x0b) { x0b = x0t; x1b = x1t; u0b = u0t; u1b = u1t; a_bl = a_tl; a_br = a_tr; }
	glBindTexture(GL_TEXTURE_2D, t.id);
	glBegin(GL_QUADS);
	glColor4f(1, 1, 1, a_tl); glTexCoord2f(u0t, vt); glVertex2f(x0t, yt);
	glColor4f(1, 1, 1, a_tr); glTexCoord2f(u1t, vt); glVertex2f(x1t, yt);
	glColor4f(1, 1, 1, a_br); glTexCoord2f(u1b, vb); glVertex2f(x1b, yb);
	glColor4f(1, 1, 1, a_bl); glTexCoord2f(u0b, vb); glVertex2f(x0b, yb);
	glEnd();
}

/* Vertical shadow fade + diagonal corner chamfer with edge fade. */
static void draw_list_item_faded(const Tex &t, float x, float y, float w, float h,
								 float list_left, float list_top,
								 float list_right, float list_bot,
								 float solid, float fade_end, float corner_cut) {
	if (!t.id || h <= 0.0f || w <= 0.0f) return;
	const float clip_top = list_top + solid;
	const float clip_bot = list_bot - solid;
	const float fade_top_end = list_top + fade_end;
	const float fade_bot_start = list_bot - fade_end;
	const float cut = corner_cut;
	/* Soften the diagonal the same way top/bottom softens (~half the cut). */
	const float feather = fminf(6.0f, cut * 0.45f);

	float y0 = y;
	float y1 = y + h;
	if (y1 <= clip_top || y0 >= clip_bot) return;
	if (y0 < clip_top) y0 = clip_top;
	if (y1 > clip_bot) y1 = clip_bot;
	if (y1 <= y0) return;

	auto emit_span = [&](float ya, float yb) {
		if (yb <= ya) return;
		const float v0 = (ya - y) / h;
		const float v1 = (yb - y) / h;
		const float at = list_edge_alpha(ya, list_top, list_bot, solid, fade_end);
		const float ab = list_edge_alpha(yb, list_top, list_bot, solid, fade_end);

		const float in_t = list_corner_inset(ya, list_top, list_bot, cut, solid);
		const float in_b = list_corner_inset(yb, list_top, list_bot, cut, solid);

		float x0t = x, x1t = x + w;
		float x0b = x, x1b = x + w;
		if (x0t < list_left + in_t) x0t = list_left + in_t;
		if (x1t > list_right - in_t) x1t = list_right - in_t;
		if (x0b < list_left + in_b) x0b = list_left + in_b;
		if (x1b > list_right - in_b) x1b = list_right - in_b;

		if (x1t <= x0t && x1b <= x0b) return;

		if (in_t <= 0.01f && in_b <= 0.01f) {
			draw_tex_vfade(t, x, ya, w, yb - ya, 0, v0, 1, v1, at, ab);
			return;
		}

		auto u_at = [&](float px) { return (px - x) / w; };

		/* Feather ONLY the sides that the chamfer actually clipped.
		 * Feathering both edges made right-side icons look like a diagonal
		 * fade (left washed out even though only the right hit the corner). */
		const bool clipped_l = (x0t > x + 0.01f) || (x0b > x + 0.01f);
		const bool clipped_r = (x1t < x + w - 0.01f) || (x1b < x + w - 0.01f);
		const float ft = clipped_l
			? fminf(feather, fmaxf(0.0f, (x1t - x0t) * 0.4f)) : 0.0f;
		const float fb = clipped_l
			? fminf(feather, fmaxf(0.0f, (x1b - x0b) * 0.4f)) : 0.0f;
		const float ft_r = clipped_r
			? fminf(feather, fmaxf(0.0f, (x1t - x0t) * 0.4f)) : 0.0f;
		const float fb_r = clipped_r
			? fminf(feather, fmaxf(0.0f, (x1b - x0b) * 0.4f)) : 0.0f;

		float i0t = x0t, i1t = x1t, i0b = x0b, i1b = x1b;

		if (ft > 0.5f || fb > 0.5f) {
			const float ft2 = fmaxf(ft, 0.5f);
			const float fb2 = fmaxf(fb, 0.5f);
			draw_tex_vfade_trap4(t,
				x0t, x0t + ft2, ya, x0b, x0b + fb2, yb,
				u_at(x0t), u_at(x0t + ft2), v0, u_at(x0b), u_at(x0b + fb2), v1,
				0.0f, at, ab, 0.0f);
			i0t = x0t + ft2;
			i0b = x0b + fb2;
		}
		if (ft_r > 0.5f || fb_r > 0.5f) {
			const float ft2 = fmaxf(ft_r, 0.5f);
			const float fb2 = fmaxf(fb_r, 0.5f);
			draw_tex_vfade_trap4(t,
				x1t - ft2, x1t, ya, x1b - fb2, x1b, yb,
				u_at(x1t - ft2), u_at(x1t), v0, u_at(x1b - fb2), u_at(x1b), v1,
				at, 0.0f, 0.0f, ab);
			i1t = x1t - ft2;
			i1b = x1b - fb2;
		}

		if (i1t > i0t || i1b > i0b)
			draw_tex_vfade_trap(t, i0t, i1t, ya, i0b, i1b, yb,
								u_at(i0t), u_at(i1t), v0, u_at(i0b), u_at(i1b), v1,
								at, ab);
	};

	/* One span across the chamfer band → a clean single diagonal edge */
	const float chamfer_top_end = list_top + solid + cut;
	const float chamfer_bot_start = list_bot - solid - cut;

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
	advance_to(chamfer_top_end);
	advance_to(chamfer_bot_start);
	advance_to(fade_bot_start);
	advance_to(y1);
}

/* Decode one UTF-8 codepoint from *p, advance *p past it.
 * Returns 0 on end-of-string or invalid sequence. */
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
	/* Skip 4-byte and invalid sequences */
	return '?';
}

/* -------------------------------------------------------------------------
 * Scale helpers
 * ---------------------------------------------------------------------- */
/* Uniform scale: sizes were designed at kRefH (1080px) height.
 * Primary axis is height (1080 → scale=1.0, 540 → scale=0.5, etc.).
 * Safety clamp: if the panel (879px wide at reference) would not fit,
 * reduce scale so it fits in win_w. */
static const float kRefPanelW = 879.0f;  /* panel_bg.bmp width */
static float ui_scale(float win_w, float win_h) {
	float s = win_h / kRefH;
	if (kRefPanelW * s > win_w) s = win_w / kRefPanelW;
	return s;
}
/* Scale a reference-space value to screen pixels */
static float sc(float ref, float scale) { return ref * scale; }

/* Draw a string using the stb_truetype atlas.
 * is_header=true uses the header bake; false uses the item bake.
 * y is the BASELINE in screen pixels (top-down ortho).
 * font_mul scales glyphs relative to the normal UI font size (1 = full size). */
static float draw_string(Font &font, const char *text, bool is_header,
					 float x, float y,
					 float cr, float cg, float cb,
					 float win_w, float win_h,
					 bool shadow = false, float shadow_dx = 0, float shadow_dy = 0,
					 float outline_px = 0, float shadow_alpha = 0.25f,
					 float text_alpha = 1.0f,
					 float fade_list_top = 0, float fade_list_bot = 0,
					 float fade_solid = 0, float fade_end = 0,
					 float font_mul = 1.0f) {
	if (!font.ok || !text || text_alpha <= 0.0f) return x;
	(void)outline_px;
	const bool do_fade = (fade_end > fade_solid);

	/* Scale factor: font was baked at reference resolution kRefW x kRefH.
	 * Use the same uniform scale as all other UI elements. */
	const float scale = ui_scale(win_w, win_h) * (font_mul > 0.0f ? font_mul : 1.0f);

	glBindTexture(GL_TEXTURE_2D, font.atlas_tex);
	/* GL_ALPHA texture + GL_MODULATE: vertex color passes through, vertex alpha
	 * is multiplied by glyph coverage. Blending (SRC_ALPHA) does the masking. */
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	stbtt_bakedchar *bc = is_header ? font.glyphs_header : font.glyphs_item;
	float fw = (float)Font::kAtlasW;
	float fh = (float)Font::kAtlasH;

	auto emit_glyph = [&](float px, float py, stbtt_bakedchar *cd, float r, float g, float b, float a) {
		float x0 = px + cd->xoff  * scale;
		float y0 = py + cd->yoff  * scale;
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
			/* Atlas covers codepoints 32..255 (Latin-1 + ASCII) */
			if (cp < 32 || cp > 255) { px += 8.0f * scale; continue; }
			stbtt_bakedchar *cd = &bc[cp - 32];
			if (cd->x0 == cd->x1) { px += cd->xadvance * scale; continue; } /* glyph missing */
			/* Fake bold: draw glyph twice with 1-pixel horizontal offset */
			emit_glyph(px,             py, cd, r, g, b, a);
			emit_glyph(px + 1.0f * scale, py, cd, r, g, b, a);
			px += cd->xadvance * scale;
		}
	};

	if (shadow)
		emit_str(shadow_dx, shadow_dy, 0, 0, 0, shadow_alpha * text_alpha);
	/* Outline: draw in black at 8 cardinal/diagonal offsets before main text */
	if (outline_px > 0.0f) {
		float o = outline_px;
		const float oa = 0.5f * text_alpha;
		emit_str(-o, -o, 0,0,0,oa); emit_str( 0,-o, 0,0,0,oa); emit_str( o,-o, 0,0,0,oa);
		emit_str(-o,  0, 0,0,0,oa);                             emit_str( o,  0, 0,0,0,oa);
		emit_str(-o,  o, 0,0,0,oa); emit_str( 0,  o, 0,0,0,oa); emit_str( o,  o, 0,0,0,oa);
	}
	emit_str(0, 0, cr, cg, cb, text_alpha);

	/* Return x after last glyph */
	float px = x;
	const char *p = text;
	unsigned int cp;
	while ((cp = utf8_next(&p)) != 0) {
		if (cp < 32 || cp > 255) { px += 8.0f * scale; continue; }
		px += bc[cp - 32].xadvance * scale;
	}
	return px + 1.0f * scale; /* +1 for fake-bold extra pixel */
}

/* Measure text width in scaled screen pixels */
static float measure_string(Font &font, const char *text, bool is_header, float win_w, float win_h,
							float font_mul = 1.0f) {
	if (!font.ok || !text) return 0;
	const float scale = ui_scale(win_w, win_h) * (font_mul > 0.0f ? font_mul : 1.0f);
	stbtt_bakedchar *bc = is_header ? font.glyphs_header : font.glyphs_item;
	float w = 0;
	const char *p = text;
	unsigned int cp;
	while ((cp = utf8_next(&p)) != 0) {
		if (cp < 32 || cp > 255) { w += 8.0f * scale; continue; }
		w += bc[cp - 32].xadvance * scale;
	}
	return w + 1.0f * scale; /* +1 for fake-bold extra pixel */
}

/* Word-wrap item labels so they don't draw under the right-side icon.
 * Prefers breaks at spaces; hard-breaks a single overlong word. Caps at
 * 2 lines (game list rows); truncates with "..." if still too long. */
static std::vector<std::string> wrap_item_label(Font &font, const char *text,
												float max_w, float win_w, float win_h) {
	std::vector<std::string> lines;
	if (!text || !text[0]) return lines;
	if (max_w <= 1.0f) { lines.push_back(text); return lines; }

	auto width_of = [&](const std::string &s) {
		return measure_string(font, s.c_str(), false, win_w, win_h);
	};
	auto fits = [&](const std::string &s) { return width_of(s) <= max_w; };

	auto append_utf8_char = [](std::string &dst, const char *&p) {
		const char *start = p;
		if (!utf8_next(&p)) return false;
		dst.append(start, (size_t)(p - start));
		return true;
	};

	auto hard_fill_line = [&](const char *&p) -> std::string {
		std::string out;
		while (*p) {
			const char *save = p;
			std::string trial = out;
			if (!append_utf8_char(trial, p)) break;
			if (!out.empty() && !fits(trial)) {
				p = save;
				break;
			}
			out.swap(trial);
		}
		return out;
	};

	const char *p = text;
	while (*p && lines.size() < 2) {
		/* Skip leading spaces between lines */
		while (*p == ' ' || *p == '\t') ++p;
		if (!*p) break;

		std::string line;
		while (*p) {
			while (*p == ' ' || *p == '\t') {
				/* Keep a single space only if line already has content and more words follow */
				const char *peek = p;
				while (*peek == ' ' || *peek == '\t') ++peek;
				if (!*peek) { p = peek; break; }
				if (line.empty()) { p = peek; continue; }
				std::string trial = line + " ";
				if (!fits(trial)) break;
				line.swap(trial);
				p = peek;
				break;
			}
			if (!*p) break;

			/* Take next word */
			const char *w0 = p;
			while (*p && *p != ' ' && *p != '\t') ++p;
			std::string word(w0, (size_t)(p - w0));
			std::string trial = line.empty() ? word : (line + " " + word);
			if (fits(trial)) {
				line.swap(trial);
				continue;
			}
			if (line.empty()) {
				/* Word alone is wider than max — hard-break it. */
				p = w0;
				line = hard_fill_line(p);
				break;
			}
			/* Word doesn't fit on this line — leave it for the next. */
			p = w0;
			break;
		}
		if (!line.empty()) lines.push_back(line);
		else break;
	}

	/* Leftover text: fold into line 2 with ellipsis. */
	while (*p == ' ' || *p == '\t') ++p;
	if (*p && !lines.empty()) {
		std::string &last = lines.back();
		std::string rest = p;
		std::string trial = last.empty() ? rest : (last + " " + rest);
		if (fits(trial)) {
			last.swap(trial);
		} else {
			std::string base = last;
			while (!base.empty()) {
				size_t i = base.size();
				while (i > 0 && ((unsigned char)base[i - 1] & 0xC0) == 0x80) --i;
				if (i == 0) break;
				base.resize(i - 1);
				while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();
				std::string cand = base + "...";
				if (fits(cand)) { last.swap(cand); break; }
			}
			if (base.empty()) last = "...";
		}
	} else if (lines.empty()) {
		lines.push_back(text);
	}
	return lines;
}

/* =========================================================================
 * Audio helpers
 * ========================================================================= */
struct Sound {
	Uint8 *buf = nullptr;
	Uint32 len = 0;
};

static Sound g_snd_check{}, g_snd_next{}, g_snd_close{}, g_snd_back{}, g_snd_confirm{}, g_snd_exit{},
			 g_snd_slider{}, g_snd_achieve_unlock{};
static SDL_AudioDeviceID g_audio_dev = 0;
static SDL_AudioSpec     g_audio_spec{};
static bool              g_sounds_loaded = false;
static bool              g_host_booted = false;

/* Cached from prefs.dat (SfxVolume 0..100, Muted 0/1). Refreshed when config /
 * quit / achievement-unlock UI is about to play sound. */
static int  g_ui_sfx_volume = 100;
static bool g_ui_sfx_muted  = false;

static void free_sound(Sound &s) {
	if (!s.buf) return;
	/* load_wav_from_rc may leave either an SDL_LoadWAV buffer or an SDL_malloc
	 * conversion buffer; SDL_free handles both allocators SDL uses here. */
	SDL_free(s.buf);
	s.buf = nullptr;
	s.len = 0;
}

static void free_all_sounds(void) {
	free_sound(g_snd_check);
	free_sound(g_snd_next);
	free_sound(g_snd_close);
	free_sound(g_snd_back);
	free_sound(g_snd_confirm);
	free_sound(g_snd_exit);
	free_sound(g_snd_slider);
	free_sound(g_snd_achieve_unlock);
	g_sounds_loaded = false;
}

static Sound load_wav_from_rc(int rc_id) {
	Sound s;
	int raw_size = 0;
	void *raw = load_raw_from_rc(rc_id, &raw_size);
	if (!raw) return s;
	SDL_RWops *rw = SDL_RWFromMem(raw, raw_size);
	if (!rw) { SDL_free(raw); return s; }
	SDL_AudioSpec spec{};
	if (!SDL_LoadWAV_RW(rw, 1, &spec, &s.buf, &s.len)) {
		SDL_free(raw);
		return s;
	}
	/* Convert to device format if needed */
	SDL_AudioCVT cvt;
	if (g_audio_dev &&
		SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
						  g_audio_spec.format, g_audio_spec.channels, g_audio_spec.freq) > 0) {
		cvt.len = (int)s.len;
		cvt.buf = (Uint8 *)SDL_malloc(cvt.len * cvt.len_mult);
		SDL_memcpy(cvt.buf, s.buf, s.len);
		SDL_FreeWAV(s.buf);
		SDL_ConvertAudio(&cvt);
		s.buf = cvt.buf;
		s.len = (Uint32)(cvt.len * cvt.len_ratio);
	}
	SDL_free(raw);
	return s;
}

static void load_ui_sounds(void) {
	if (g_sounds_loaded) return;
	free_all_sounds();
	g_snd_check   = load_wav_from_rc(RC_CFG_SND_CHECK);
	g_snd_next    = load_wav_from_rc(RC_CFG_SND_NEXT);
	g_snd_close   = load_wav_from_rc(RC_CFG_SND_CLOSE);
	g_snd_back    = load_wav_from_rc(RC_CFG_SND_BACK);
	g_snd_confirm = load_wav_from_rc(RC_CFG_SND_CONFIRM);
	g_snd_exit    = load_wav_from_rc(RC_CFG_SND_EXIT);
	g_snd_slider  = load_wav_from_rc(RC_CFG_SND_SLIDER);
	g_snd_achieve_unlock = load_wav_from_rc(RC_CFG_SND_ACHIEVE_UNLOCK);
	g_sounds_loaded = true;
}

/* Opens the dedicated UI click device. Mirrors audio_output.cpp: the process
 * starts with SDL_INIT_VIDEO only, so AUDIO must be brought up explicitly.
 * Safe to call repeatedly; retries if a previous attempt failed (e.g. quit
 * confirm shown during boot before AUDIO was ready). */
static bool ensure_ui_audio(void) {
	if (g_audio_dev) return true;

	if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
		return false;

	SDL_AudioSpec want{};
	want.freq     = 44100;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 1024;
	want.callback = nullptr;
	g_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &g_audio_spec, 0);
	if (!g_audio_dev) return false;
	SDL_PauseAudioDevice(g_audio_dev, 0);

	/* Device format may differ from a previous failed/partial load — reload. */
	g_sounds_loaded = false;
	load_ui_sounds();
	return true;
}

/* Drop the UI audio device. Always safe; used before the game claims the sink
 * (pre-boot) and again on process shutdown before SDL_Quit. */
static void close_ui_audio_device(void) {
	if (!g_audio_dev) return;
	const Uint32 start = SDL_GetTicks();
	while (SDL_GetQueuedAudioSize(g_audio_dev) > 0 &&
		   (SDL_GetTicks() - start) < 1000) {
		SDL_Delay(10);
	}
	SDL_CloseAudioDevice(g_audio_dev);
	g_audio_dev = 0;
	free_all_sounds();
}

/* Drop the UI device before the game claims the sink (pre-boot only). */
static void release_ui_audio_for_game(void) {
	if (g_host_booted) return;
	close_ui_audio_device();
}

static void refresh_ui_sfx_from_prefs(void) {
	pvz2native::dex::config_store_reload();
	std::int64_t vol = 100;
	std::int64_t muted = 0;
	pvz2native::dex::config_store_get_int64("SfxVolume", vol);
	pvz2native::dex::config_store_get_int64("Muted", muted);
	if (vol < 0) vol = 0;
	if (vol > 100) vol = 100;
	g_ui_sfx_volume = (int)vol;
	g_ui_sfx_muted = (muted != 0);
}

static void play_sound(const Sound &s) {
	if (g_ui_sfx_muted || g_ui_sfx_volume <= 0) return;
	if (!ensure_ui_audio()) return;
	/* ensure_ui_audio may have just reloaded the global samples; re-read via
	 * the caller's reference (it aliases g_snd_*). */
	if (!s.buf || !s.len) return;
	SDL_ClearQueuedAudio(g_audio_dev);

	if (g_ui_sfx_volume >= 100) {
		SDL_QueueAudio(g_audio_dev, s.buf, s.len);
		return;
	}

	/* Scale PCM to SfxVolume (0..100). Device is opened as S16 stereo. */
	Uint8 *scaled = (Uint8 *)SDL_malloc(s.len);
	if (!scaled) {
		SDL_QueueAudio(g_audio_dev, s.buf, s.len);
		return;
	}
	SDL_memset(scaled, 0, s.len);
	const int mix_vol = (g_ui_sfx_volume * SDL_MIX_MAXVOLUME) / 100;
	SDL_MixAudioFormat(scaled, s.buf, g_audio_spec.format, s.len, mix_vol);
	SDL_QueueAudio(g_audio_dev, scaled, s.len);
	SDL_free(scaled);
}

/* =========================================================================
 * Config data mirrored for editing
 * ========================================================================= */
struct EditConfig {
	int  fullscreen   = 1;
	int  video_width  = 0;
	int  video_height = 0;
	int  video_display = 0;
	int  fps_limit    = 60;
	char locale[32]   = "en_US";
	char cur_symbol[8]  = "$";
	char cur_code[8]    = "USD";
	int  emulate_iap  = 0;
	int  achievement_notifications = 1;

	void load_from_live() {
		const pvz2_config_t *c = pvz2_config();
		fullscreen   = c->video_fullscreen;
		video_width  = c->video_width;
		video_height = c->video_height;
		video_display = pvz2_effective_display();
		fps_limit    = c->fps_limit;
		SDL_strlcpy(locale,     c->user_locale,      sizeof(locale));
		SDL_strlcpy(cur_symbol, c->currency_symbol,  sizeof(cur_symbol));
		SDL_strlcpy(cur_code,   c->currency_code,    sizeof(cur_code));
		emulate_iap  = c->emulate_iap;
		achievement_notifications = c->achievement_notifications;
	}

	bool differs_from_live() const {
		const pvz2_config_t *c = pvz2_config();
		return fullscreen  != c->video_fullscreen ||
			   video_width != c->video_width      ||
			   video_height!= c->video_height     ||
			   video_display != pvz2_effective_display() ||
			   fps_limit   != c->fps_limit         ||
			   strcmp(locale,     c->user_locale)     != 0 ||
			   strcmp(cur_symbol, c->currency_symbol)  != 0 ||
			   strcmp(cur_code,   c->currency_code)    != 0 ||
			   emulate_iap != c->emulate_iap ||
			   achievement_notifications != c->achievement_notifications;
	}
};

/* =========================================================================
 * Menu navigation
 * ========================================================================= */
enum class Screen { Main, Resolution, Fps, Language, Currency, Bindings, BindEdit,
                    GamepadBinds, GamepadBindPick, GamepadDevicePick, DisplayPick };

/* =========================================================================
 * Global state
 * ========================================================================= */
namespace {

bool g_open = false;
bool g_open_requested = false;
bool g_quit_confirm = false;
SDL_Window *g_window = nullptr;

Tex g_tex_panel{}, g_tex_list_bg{}, g_tex_list_item{};
Tex g_tex_close_up{}, g_tex_close_down{};
Tex g_tex_back_up{}, g_tex_back_down{};
Tex g_tex_arrow_up{}, g_tex_arrow_down{};
Tex g_tex_exit_up{}, g_tex_exit_down{};
Tex g_tex_cancel_up{}, g_tex_cancel_down{};
Tex g_tex_panel_exit{};
Tex g_tex_chk_on{}, g_tex_chk_off{};
Tex g_tex_scroll_track{}, g_tex_scroll_bar{};
Tex g_tex_slider_thumb{}, g_tex_slider_empty{}, g_tex_slider_full{};

/* Bind-editor textures */
Tex g_tex_edit_up{}, g_tex_edit_down{};
Tex g_tex_save_up{}, g_tex_save_down{};
Tex g_tex_bind_input{};
Tex g_tex_bind_container{};
Tex g_tex_bind_preview{};
Tex g_tex_point_green{};

Font g_font;

/* Bind-editor state */
int  g_edit_bind_idx   = -1;        /* index into pvz2_config()->shortcuts being edited */
char g_captured_key[64] = "";       /* key name captured during BindEdit screen */
bool g_save_pressed    = false;

/* Gamepad bind editor: which remappable button is being assigned ("b","x",...). */
char g_edit_gamepad_btn[16] = "";

/* point_green pulse animation */
Uint32 g_pulse_start_ms = 0;        /* timestamp when the current pulse began */
static const Uint32 kPulseDurMs = 700; /* one pulse cycle duration */

bool g_loaded = false;

Screen g_screen = Screen::Main;
EditConfig g_edit;

/* Mouse state */
float g_mouse_x = 0, g_mouse_y = 0;
bool  g_mouse_down = false;

/* Close/back button pressed state */
bool g_close_pressed = false;
bool g_back_pressed  = false;

/* Quit-confirm button pressed state */
bool g_quit_btn_pressed = false;
bool g_cancel_btn_pressed = false;

/* Arrow item pressed index (-1 = none) */
int  g_arrow_pressed_idx = -1;

/* Scroll state */
float g_scroll_offset = 0.0f;  /* pixels scrolled */

/* Scrollbar drag state */
bool  g_scrollbar_dragging      = false;
float g_scrollbar_drag_mouse_y0 = 0.f;  /* mouse y at drag start */
float g_scrollbar_drag_scroll0  = 0.f;  /* scroll offset at drag start */

/* Touch-like list drag (click-drag on items to scroll) */
bool  g_list_drag_armed   = false; /* mousedown on list, not yet past threshold */
bool  g_list_dragging     = false; /* actively scrolling the list */
float g_list_drag_mouse_y0 = 0.f;
float g_list_drag_scroll0  = 0.f;
static const float kListDragThreshold = 8.0f; /* px before a press becomes a drag */

/* Slider drag: which GamepadBinds slider row is being scrubbed (-1 = none). */
enum class SliderId : int { None = -1, StickSpeed = 0, StickDeadzone = 1 };
int g_slider_drag = (int)SliderId::None;

/* Shared currency options: symbol is what Info_SysGetUserCurrencySymbol returns.
 * Use explicit UTF-8 bytes (not source-charset-dependent literals) so MSVC
 * always embeds € / £ correctly regardless of /utf-8. */
static const char kSymUsd[]  = {'$', '\0'};
static const char kSymEur[]  = {'\xE2', '\x82', '\xAC', '\0'}; /* € */
static const char kSymGbp[]  = {'\xC2', '\xA3', '\0'};         /* £ */
static const char kSymBrl[]  = {'R', '$', '\0'};
static const char kSymMxn[]  = {'$', '\0'};

struct CurOpt {
	const char *sym;
	const char *code;
};
static const CurOpt kCurrencyOpts[] = {
	{kSymUsd, "USD"},
	{kSymEur, "EUR"},
	{kSymGbp, "GBP"},
	{kSymBrl, "BRL"},
	{kSymMxn, "MXN"},
};
static const int kCurrencyOptCount = (int)(sizeof(kCurrencyOpts) / sizeof(kCurrencyOpts[0]));

/* Resolution / FPS catalogs. Filtered at runtime against display 0 capabilities;
 * the currently selected value is always kept visible even if it exceeds the
 * monitor, so an existing config.ini choice is not silently dropped. */
struct ResOpt {
	const char *label; /* nullptr => localized "Fullscreen" */
	int w, h, fs;
};

static const ResOpt kAllResOpts[] = {
	{nullptr,        0,    0,    1},
	{"800x600",    800,  600,  0},
	{"1024x768",  1024,  768,  0},
	{"1280x720",  1280,  720,  0},
	{"1366x768",  1366,  768,  0},
	{"1280x1024", 1280, 1024,  0},
	{"1440x900",  1440,  900,  0},
	{"1600x900",  1600,  900,  0},
	{"1920x1080", 1920, 1080,  0},
	{"1920x1200", 1920, 1200,  0},
	{"2560x1080", 2560, 1080,  0},
	{"2560x1440", 2560, 1440,  0},
	{"3440x1440", 3440, 1440,  0},
	{"3840x2160", 3840, 2160,  0},
};

/* Finite FPS caps; 0 = unlimited and is always offered. */
static const int kAllFpsOpts[] = {30, 60, 120, 0};

struct DisplayCaps {
	int max_w;
	int max_h;
	int refresh_hz;
};

static DisplayCaps query_display0_caps(void) {
	DisplayCaps cap{1920, 1080, 60};
	SDL_DisplayMode dm{};
	if (SDL_GetCurrentDisplayMode(pvz2_effective_display(), &dm) == 0) {
		if (dm.w > 0) cap.max_w = dm.w;
		if (dm.h > 0) cap.max_h = dm.h;
		if (dm.refresh_rate > 0) cap.refresh_hz = dm.refresh_rate;
	}
	return cap;
}

static bool res_opt_is_current(const ResOpt &o, int cur_fs, int cur_w, int cur_h) {
	if (o.fs) return cur_fs != 0;
	return !cur_fs && cur_w == o.w && cur_h == o.h;
}

static std::vector<ResOpt> filtered_res_opts(int cur_fs, int cur_w, int cur_h) {
	const DisplayCaps cap = query_display0_caps();
	std::vector<ResOpt> out;
	out.reserve(sizeof(kAllResOpts) / sizeof(kAllResOpts[0]));
	for (const ResOpt &o : kAllResOpts) {
		const bool fits = o.fs || (o.w <= cap.max_w && o.h <= cap.max_h);
		if (fits || res_opt_is_current(o, cur_fs, cur_w, cur_h))
			out.push_back(o);
	}
	return out;
}

static std::vector<int> filtered_fps_opts(int cur_fps) {
	const DisplayCaps cap = query_display0_caps();
	std::vector<int> out;
	out.reserve(sizeof(kAllFpsOpts) / sizeof(kAllFpsOpts[0]));
	for (int v : kAllFpsOpts) {
		if (v == 0) { out.push_back(v); continue; } /* unlimited always */
		if (v <= cap.refresh_hz || v == cur_fps)
			out.push_back(v);
	}
	return out;
}

/* -------------------------------------------------------------------------
 * Item (list row) descriptor – shared between draw and hit-test helpers
 * ---------------------------------------------------------------------- */
struct Item {
	std::string label;
	bool is_arrow;           /* true = opens sub-screen (arrow icon) */
	bool is_edit;            /* true = opens bind-edit screen (edit icon) */
	bool checked;            /* valid when is_arrow==false && is_edit==false && !is_slider */
	std::string right_label; /* sub-value shown next to arrow items   */
	bool is_slider = false;  /* stick speed/deadzone slider control */
	int slider_id = -1;      /* SliderId when is_slider */
	float slider_t = 0.0f;   /* 0..1 normalized thumb position when is_slider */
};

/* -------------------------------------------------------------------------
 * Slide transition state
 * ---------------------------------------------------------------------- */
static const Uint32 kTransDurMs = 220;  /* milliseconds for one slide */

Screen  g_prev_screen    = Screen::Main;
float   g_prev_scroll    = 0.0f;
bool    g_in_transition  = false;
bool    g_trans_forward  = true;   /* true = forward (→sub), false = back (←main) */
Uint32  g_trans_start_ms = 0;
float   g_trans_t        = 1.0f;   /* normalised [0,1], 1 = complete */

/* Saved scroll position per screen – restored when navigating back */
static const int kNumScreens = 11; /* must match Screen enum count */
float   g_saved_scroll[kNumScreens] = {};

/* Remappable pad buttons shown in Gamepad Configuration (Start/A/Back/L-R fixed). */
static const char *kGamepadRemapButtons[] = {"b", "x", "y", "l3", "r3"};
static const int kGamepadRemapCount =
    (int)(sizeof(kGamepadRemapButtons) / sizeof(kGamepadRemapButtons[0]));

struct DetectedPad {
	int index = 0;
	std::string name;
};

static std::vector<DetectedPad> list_detected_gamepads() {
	std::vector<DetectedPad> out;
	const int n = SDL_NumJoysticks();
	for (int i = 0; i < n; ++i) {
		if (!SDL_IsGameController(i)) continue;
		const char *name = SDL_JoystickNameForIndex(i);
		out.push_back({i, name && name[0] ? name : "?"});
	}
	return out;
}

static std::string selected_gamepad_status(const Strings *str) {
	const pvz2_config_t *cfg = pvz2_config();
	if (!cfg->gamepad_enabled) return str->gamepad_disabled;
	const int n = SDL_NumJoysticks();
	if (cfg->gamepad_port >= 0 && cfg->gamepad_port < n) {
		const char *name = SDL_JoystickNameForIndex(cfg->gamepad_port);
		if (name && name[0]) return name;
	}
	/* Enabled but the preferred index is gone — still show something useful. */
	char buf[32];
	SDL_snprintf(buf, sizeof(buf), "#%d", cfg->gamepad_port);
	return buf;
}

struct DetectedDisplay {
	int index = 0;
	std::string name;
};

static std::vector<DetectedDisplay> list_detected_displays() {
	std::vector<DetectedDisplay> out;
	const int n = SDL_GetNumVideoDisplays();
	for (int i = 0; i < n; ++i) {
		const char *name = SDL_GetDisplayName(i);
		std::string label;
		if (name && name[0]) {
			label = name;
		} else {
			char buf[32];
			SDL_snprintf(buf, sizeof(buf), "Display %d", i);
			label = buf;
		}
		out.push_back({i, label});
	}
	return out;
}

/* Current [video] display, shown as the Main-screen row value. Reflects the
 * pending edit (not yet applied until Save/close), same as the other rows. */
static std::string selected_display_status() {
	const int idx = g_edit.video_display;
	const char *name = SDL_GetDisplayName(idx);
	if (name && name[0]) return name;
	char buf[32];
	SDL_snprintf(buf, sizeof(buf), "Display %d", idx);
	return buf;
}

static const char *gamepad_button_label(const char *btn, const Strings *str) {
	if (!btn || !str) return "";
	if (SDL_strcasecmp(btn, "b") == 0) return str->gamepad_btn_b;
	if (SDL_strcasecmp(btn, "x") == 0) return str->gamepad_btn_x;
	if (SDL_strcasecmp(btn, "y") == 0) return str->gamepad_btn_y;
	if (SDL_strcasecmp(btn, "l3") == 0) return str->gamepad_btn_l3;
	if (SDL_strcasecmp(btn, "r3") == 0) return str->gamepad_btn_r3;
	return btn;
}

static const char *find_gamepad_bind_name(const char *button) {
	const pvz2_config_t *cfg = pvz2_config();
	for (unsigned i = 0; i < cfg->gamepad_bind_count; ++i) {
		if (SDL_strcasecmp(cfg->gamepad_binds[i].button, button) == 0)
			return cfg->gamepad_binds[i].bind_name;
	}
	return "";
}

static bool gamepad_bind_names_equal(const char *a, const char *b) {
	if ((!a || !a[0]) && (!b || !b[0])) return true;
	if (!a || !a[0] || !b || !b[0]) return false;
	const char *aa = a;
	const char *bb = b;
	if (std::strncmp(aa, "bind_", 5) == 0) aa += 5;
	if (std::strncmp(bb, "bind_", 5) == 0) bb += 5;
	return SDL_strcasecmp(aa, bb) == 0;
}

static float clampf(float v, float lo, float hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static float stick_speed_to_t(float v) {
	return clampf((v - kStickSpeedMin) / (kStickSpeedMax - kStickSpeedMin), 0.0f, 1.0f);
}
static float stick_speed_from_t(float t) {
	return kStickSpeedMin + clampf(t, 0.0f, 1.0f) * (kStickSpeedMax - kStickSpeedMin);
}
static float stick_deadzone_to_t(float v) {
	return clampf((v - kStickDeadzoneMin) / (kStickDeadzoneMax - kStickDeadzoneMin), 0.0f, 1.0f);
}
static float stick_deadzone_from_t(float t) {
	return kStickDeadzoneMin + clampf(t, 0.0f, 1.0f) * (kStickDeadzoneMax - kStickDeadzoneMin);
}

/* Track rect inside a list_item row (screen pixels). */
static void slider_track_rect(float item_x, float item_y, float item_w, float item_h,
							  float s, float &tx, float &ty, float &tw, float &th) {
	tw = sc((float)g_tex_slider_empty.w, s);
	th = sc((float)g_tex_slider_empty.h, s);
	tx = item_x + item_w - sc(kSliderFromRight, s) - tw;
	ty = item_y + (item_h - th) * 0.5f;
}

static float slider_t_from_mouse_x(float mx, float track_x, float track_w) {
	if (track_w <= 1.0f) return 0.0f;
	/* Action range: first pixel .. last pixel of the track. */
	return clampf((mx - track_x) / (track_w - 1.0f), 0.0f, 1.0f);
}

static void apply_slider_t(int slider_id, float t, bool persist) {
	t = clampf(t, 0.0f, 1.0f);
	if (slider_id == (int)SliderId::StickSpeed) {
		pvz2_config_update_gamepad_stick_param(0, stick_speed_from_t(t), persist ? 1 : 0);
	} else if (slider_id == (int)SliderId::StickDeadzone) {
		pvz2_config_update_gamepad_stick_param(1, stick_deadzone_from_t(t), persist ? 1 : 0);
	}
	pvz2_gamepad_reload();
}

static void draw_item_slider(float item_x, float iy, float item_w, float item_h,
							 float s, float t) {
	if (!g_tex_slider_empty.id) return;
	float tx, ty, tw, th;
	slider_track_rect(item_x, iy, item_w, item_h, s, tx, ty, tw, th);
	t = clampf(t, 0.0f, 1.0f);

	draw_tex(g_tex_slider_empty, tx, ty, tw, th);

	const float thumb_cx = tx + t * (tw - 1.0f);
	float full_w = thumb_cx - tx;
	if (full_w > tw) full_w = tw;
	if (full_w > 0.5f && g_tex_slider_full.id) {
		const float u1 = full_w / tw;
		draw_tex_hcrop(g_tex_slider_full, tx, ty, full_w, th, 0.0f, u1);
	}

	if (!g_tex_slider_thumb.id) return;
	const float thw = sc((float)g_tex_slider_thumb.w, s);
	const float thh = sc((float)g_tex_slider_thumb.h, s);
	const float thumb_cy = ty + th * 0.5f;
	/* t=0 → 0°, t=1 → +400° with the clockwise screen matrix above. */
	const float deg = t * kSliderSpinDeg;
	const float sh = sc(kSliderThumbShadowPx, s);
	/* Soft shadow offset down-right, then the spinning thumb. */
	draw_tex_rotated(g_tex_slider_thumb, thumb_cx + sh, thumb_cy + sh, thw, thh,
					 deg, 0.0f, 0.0f, 0.0f, 0.35f);
	draw_tex_rotated(g_tex_slider_thumb, thumb_cx, thumb_cy, thw, thh,
					 deg, 1.0f, 1.0f, 1.0f, 1.0f);
}

/* -------------------------------------------------------------------------
 * Build item list for a given screen (shared by draw and hit-test)
 * ---------------------------------------------------------------------- */
static std::vector<Item> build_items(Screen screen, const Strings *str) {
    std::vector<Item> items;
    if (screen == Screen::Main) {
        items.push_back({str->display_config, true, false, false, selected_display_status()});

        std::string res_val;
        if (g_edit.fullscreen) res_val = str->fullscreen;
        else {
            char buf[32];
            SDL_snprintf(buf, sizeof(buf), "%dx%d", g_edit.video_width, g_edit.video_height);
            res_val = buf;
        }
        items.push_back({str->resolution, true, false, false, res_val});

        std::string fps_val;
        if (g_edit.fps_limit <= 0) fps_val = str->fps_unlimited;
        else { char buf[16]; SDL_snprintf(buf,sizeof(buf),"%d fps",g_edit.fps_limit); fps_val=buf; }
        items.push_back({str->fps, true, false, false, fps_val});

        const char *lang_name = str->lang_en;
        if      (strncmp(g_edit.locale,"es",2)==0) lang_name=str->lang_es;
        else if (strncmp(g_edit.locale,"fr",2)==0) lang_name=str->lang_fr;
        else if (strncmp(g_edit.locale,"de",2)==0) lang_name=str->lang_de;
        else if (strncmp(g_edit.locale,"pt",2)==0) lang_name=str->lang_pt;
        else if (strncmp(g_edit.locale,"it",2)==0) lang_name=str->lang_it;
        items.push_back({str->language, true, false, false, lang_name});

        const char *cur_name = str->cur_usd;
        if      (strcmp(g_edit.cur_code,"EUR")==0) cur_name=str->cur_eur;
        else if (strcmp(g_edit.cur_code,"GBP")==0) cur_name=str->cur_gbp;
        else if (strcmp(g_edit.cur_code,"BRL")==0) cur_name=str->cur_brl;
        else if (strcmp(g_edit.cur_code,"MXN")==0||strcmp(g_edit.cur_code,"ARS")==0) cur_name=str->cur_peso;
        items.push_back({str->currency, true, false, false, cur_name});

        items.push_back({str->input_config, true, false, false, ""});
        items.push_back({str->gamepad_config, true, false, false, ""});
        items.push_back({str->view_achievements, true, false, false, ""});
        items.push_back({str->achievement_notify, false, false,
                         g_edit.achievement_notifications != 0, ""});
        items.push_back({str->iap, false, false, g_edit.emulate_iap != 0, ""});
        items.push_back({str->exit_program, true, false, false, ""});

    } else if (screen == Screen::Resolution) {
        const std::vector<ResOpt> opts = filtered_res_opts(
			g_edit.fullscreen, g_edit.video_width, g_edit.video_height);
        for (const ResOpt &o : opts) {
            bool sel;
            if (o.fs) sel = (g_edit.fullscreen != 0);
            else      sel = (!g_edit.fullscreen && g_edit.video_width==o.w && g_edit.video_height==o.h);
            std::string lbl = o.label ? o.label : str->fullscreen;
            items.push_back({lbl, false, false, sel, ""});
        }
    } else if (screen == Screen::Fps) {
        const std::vector<int> fps_vals = filtered_fps_opts(g_edit.fps_limit);
        for (int v : fps_vals) {
            std::string lbl;
            if (v <= 0) lbl = str->fps_unlimited;
            else { char buf[32]; SDL_snprintf(buf, sizeof(buf), "%d fps", v); lbl = buf; }
            items.push_back({lbl, false, false, g_edit.fps_limit == v, ""});
        }

    } else if (screen == Screen::Language) {
        auto opts = std::vector<std::pair<const char*,const char*>>{
            {str->lang_en, "en_US"},
            {str->lang_es, "es_ES"},
            {str->lang_fr, "fr_FR"},
            {str->lang_de, "de_DE"},
            {str->lang_pt, "pt_BR"},
            {str->lang_it, "it_IT"},
        };
        for (auto &o : opts)
            items.push_back({o.first, false, false, strcmp(g_edit.locale,o.second)==0, ""});

    } else if (screen == Screen::Currency) {
        const char *labels3[] = {str->cur_usd,str->cur_eur,str->cur_gbp,str->cur_brl,str->cur_peso};
        for (int i = 0; i < kCurrencyOptCount; ++i)
            items.push_back({labels3[i], false, false,
                             strcmp(g_edit.cur_code, kCurrencyOpts[i].code)==0, ""});
    } else if (screen == Screen::Bindings) {
        /* One edit-icon row per bind entry */
        const pvz2_config_t *cfg = pvz2_config();
        for (unsigned i = 0; i < cfg->shortcut_count; ++i) {
            std::string display = bind_display_name(cfg->shortcuts[i].name, g_edit.locale);
            /* Append : KEY (uppercase) */
            std::string key = cfg->shortcuts[i].key;
            if (!key.empty()) {
                for (char &c : key) c = (char)std::toupper((unsigned char)c);
                display += ": " + key;
            }
            items.push_back({display, false, true, false, ""});
        }
    } else if (screen == Screen::GamepadBinds) {
        items.push_back({str->gamepad_selected, true, false, false,
                         selected_gamepad_status(str)});
        {
            const pvz2_config_t *cfg = pvz2_config();
            Item speed{};
            speed.label = str->gamepad_stick_speed;
            speed.is_slider = true;
            speed.slider_id = (int)SliderId::StickSpeed;
            speed.slider_t = stick_speed_to_t(cfg->gamepad_stick_speed);
            items.push_back(speed);
            Item dead{};
            dead.label = str->gamepad_stick_deadzone;
            dead.is_slider = true;
            dead.slider_id = (int)SliderId::StickDeadzone;
            dead.slider_t = stick_deadzone_to_t(cfg->gamepad_stick_deadzone);
            items.push_back(dead);
        }
        for (int i = 0; i < kGamepadRemapCount; ++i) {
            const char *btn = kGamepadRemapButtons[i];
            std::string display = gamepad_button_label(btn, str);
            const char *bound = find_gamepad_bind_name(btn);
            display += ": ";
            if (bound && bound[0] && SDL_strcasecmp(bound, "NULL") != 0) {
                const char *id = bound;
                if (std::strncmp(id, "bind_", 5) == 0) id += 5;
                display += bind_display_name(id, g_edit.locale);
            } else {
                display += str->gamepad_none;
            }
            items.push_back({display, false, true, false, ""});
        }
    } else if (screen == Screen::GamepadDevicePick) {
        const pvz2_config_t *cfg = pvz2_config();
        items.push_back({str->gamepad_disabled, false, false, cfg->gamepad_enabled == 0, ""});
        const std::vector<DetectedPad> pads = list_detected_gamepads();
        for (const DetectedPad &p : pads) {
            const bool sel = cfg->gamepad_enabled != 0 && cfg->gamepad_port == p.index;
            items.push_back({p.name, false, false, sel, ""});
        }
    } else if (screen == Screen::DisplayPick) {
        const int effective = g_edit.video_display;
        std::vector<DetectedDisplay> displays = list_detected_displays();
        if (displays.empty()) displays.push_back({0, std::string("Display 0")});
        for (const DetectedDisplay &dd : displays) {
            items.push_back({dd.name, false, false, dd.index == effective, ""});
        }
    } else if (screen == Screen::GamepadBindPick) {
        /* Row 0 = None; then one checkbox per keyboard shortcut (radio-style). */
        const char *cur = find_gamepad_bind_name(g_edit_gamepad_btn);
        const bool none_sel = !cur || !cur[0] || SDL_strcasecmp(cur, "NULL") == 0;
        items.push_back({str->gamepad_none, false, false, none_sel, ""});
        const pvz2_config_t *cfg = pvz2_config();
        for (unsigned i = 0; i < cfg->shortcut_count; ++i) {
            std::string display = bind_display_name(cfg->shortcuts[i].name, g_edit.locale);
            bool sel = gamepad_bind_names_equal(cur, cfg->shortcuts[i].name);
            items.push_back({display, false, false, sel, ""});
        }
    }
    return items;
}

/* -------------------------------------------------------------------------
 * Resource loading
 * ---------------------------------------------------------------------- */
static void load_resources(float win_w, float win_h) {
	if (g_loaded) return;

	g_tex_panel       = load_bmp_from_rc(RC_CFG_PANEL_BG);
	g_tex_list_bg     = load_bmp_from_rc(RC_CFG_LIST_BG);
	g_tex_list_item   = load_bmp_from_rc(RC_CFG_LIST_ITEM);
	g_tex_close_up    = load_bmp_from_rc(RC_CFG_BTN_CLOSE_UP);
	g_tex_close_down  = load_bmp_from_rc(RC_CFG_BTN_CLOSE_DOWN);
	g_tex_back_up     = load_bmp_from_rc(RC_CFG_BTN_BACK_UP);
	g_tex_back_down   = load_bmp_from_rc(RC_CFG_BTN_BACK_DOWN);
	g_tex_arrow_up    = load_bmp_from_rc(RC_CFG_BTN_ARROW_UP);
	g_tex_arrow_down  = load_bmp_from_rc(RC_CFG_BTN_ARROW_DOWN);
	g_tex_exit_up     = load_bmp_from_rc(RC_CFG_BTN_EXIT_UP);
	g_tex_exit_down   = load_bmp_from_rc(RC_CFG_BTN_EXIT_DOWN);
	g_tex_cancel_up   = load_bmp_from_rc(RC_CFG_BTN_CANCEL_UP);
	g_tex_cancel_down = load_bmp_from_rc(RC_CFG_BTN_CANCEL_DOWN);
	g_tex_panel_exit  = load_bmp_from_rc(RC_CFG_PANEL_EXIT_BG);
	g_tex_chk_on      = load_bmp_from_rc(RC_CFG_CHK_CHECKED);
	g_tex_chk_off     = load_bmp_from_rc(RC_CFG_CHK_UNCHECKED);
	g_tex_scroll_track= load_bmp_from_rc(RC_CFG_SCROLL_TRACK);
	g_tex_scroll_bar  = load_bmp_from_rc(RC_CFG_SCROLL_BAR);
	g_tex_slider_thumb= load_bmp_from_rc(RC_CFG_SLIDER_THUMB);
	g_tex_slider_empty= load_bmp_from_rc(RC_CFG_SLIDER_EMPTY);
	g_tex_slider_full = load_bmp_from_rc(RC_CFG_SLIDER_FULL);

	/* Load font: px sizes are fixed atlas values, ortho scales them on screen */
	g_font.load(RC_CFG_FONT, kHeaderFontPx, kItemFontPx);

	/* UI click samples are loaded lazily in ensure_ui_audio() so a quit dialog
	 * during boot can Init AUDIO on demand and release the device before the
	 * game's OpenSL sink claims the output. */

	g_tex_edit_up       = load_bmp_from_rc(RC_CFG_BTN_EDIT_UP);
	g_tex_edit_down     = load_bmp_from_rc(RC_CFG_BTN_EDIT_DOWN);
	g_tex_save_up       = load_bmp_from_rc(RC_CFG_BTN_SAVE_UP);
	g_tex_save_down     = load_bmp_from_rc(RC_CFG_BTN_SAVE_DOWN);
	g_tex_bind_input    = load_bmp_from_rc(RC_CFG_TXT_BIND_INPUT);
	g_tex_bind_container= load_bmp_from_rc(RC_CFG_IMG_BIND_CONT);
	g_tex_bind_preview  = load_bmp_from_rc(RC_CFG_IMG_BIND_PREV);
	g_tex_point_green   = load_bmp_from_rc(RC_CFG_POINT_GREEN);

	g_loaded = true;
}

/* -------------------------------------------------------------------------
 * Config persistence + live apply (relaunch when locale or emulate_iap changes)
 * ---------------------------------------------------------------------- */
static void write_overlay_settings_to_ini(void) {
	char *base = pvz2_host_data_path();
	if (!base) return;
	char ini_path[1024];
	SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base);
	SDL_free(base);

	std::vector<std::string> lines;
	{
		FILE *f = fopen(ini_path, "rb");
		if (f) {
			char buf[512];
			while (fgets(buf, sizeof(buf), f))
				lines.push_back(buf);
			fclose(f);
		}
	}

	auto set_key = [&](const char *section, const char *key, const char *value) {
		std::string sec_hdr = std::string("[") + section + "]";
		bool in_sec = false;
		bool found  = false;
		for (auto &l : lines) {
			std::string trimmed = l;
			while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
				trimmed.pop_back();
			if (trimmed == sec_hdr) { in_sec = true; continue; }
			if (!trimmed.empty() && trimmed[0] == '[') { in_sec = false; continue; }
			if (in_sec) {
				std::string stripped = trimmed;
				while (!stripped.empty() && (stripped[0] == ';' || stripped[0] == ' '))
					stripped = stripped.substr(1);
				/* Exact key match: must be followed by end, whitespace, or '='.
				 * (A plain prefix match would be fragile with similarly named keys.) */
				const size_t klen = std::strlen(key);
				if (stripped.size() >= klen && stripped.compare(0, klen, key) == 0) {
					const char next = (stripped.size() == klen) ? '\0' : stripped[klen];
					if (next == '\0' || next == '=' || next == ' ' || next == '\t') {
						size_t eq = stripped.find('=');
						if (eq != std::string::npos) {
							l = std::string(key) + " = " + value + "\n";
							found = true;
							break;
						}
					}
				}
			}
		}
		if (!found) {
			bool inserted = false;
			for (size_t i = 0; i < lines.size(); ++i) {
				std::string t = lines[i];
				while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
				if (t == sec_hdr) {
					lines.insert(lines.begin() + (int)i + 1,
								 std::string(key) + " = " + value + "\n");
					inserted = true;
					break;
				}
			}
			if (!inserted) {
				lines.push_back("\n[" + std::string(section) + "]\n");
				lines.push_back(std::string(key) + " = " + value + "\n");
			}
		}
	};

	char tmp[64];

	SDL_snprintf(tmp, sizeof(tmp), "%d", g_edit.fullscreen);
	set_key("video", "fullscreen", tmp);

	if (g_edit.fullscreen) {
		set_key("video", "width",  "0");
		set_key("video", "height", "0");
	} else {
		SDL_snprintf(tmp, sizeof(tmp), "%d", g_edit.video_width);
		set_key("video", "width", tmp);
		SDL_snprintf(tmp, sizeof(tmp), "%d", g_edit.video_height);
		set_key("video", "height", tmp);
	}

	SDL_snprintf(tmp, sizeof(tmp), "%d", g_edit.fps_limit);
	set_key("video", "fps_limit", tmp);

	SDL_snprintf(tmp, sizeof(tmp), "%d", g_edit.video_display);
	set_key("video", "display", tmp);

	set_key("game", "user_locale",      g_edit.locale);
	set_key("game", "currency_symbol",  g_edit.cur_symbol);
	set_key("game", "currency_code",    g_edit.cur_code);
	SDL_snprintf(tmp, sizeof(tmp), "%d", g_edit.emulate_iap);
	set_key("game", "emulate_iap", tmp);
	SDL_snprintf(tmp, sizeof(tmp), "%d", g_edit.achievement_notifications);
	set_key("game", "achievement_notifications", tmp);

	FILE *f = fopen(ini_path, "wb");
	if (f) {
		for (const auto &l : lines) fputs(l.c_str(), f);
		fclose(f);
	}
}

static void relaunch_process(void) {
#ifdef _WIN32
	wchar_t exe_path[MAX_PATH];
	GetModuleFileNameW(NULL, exe_path, MAX_PATH);
	ShellExecuteW(NULL, L"open", exe_path, NULL, NULL, SW_SHOWNORMAL);
#else
	/* Spawn a fresh process of the same binary, then exit this one — same
	 * idea as ShellExecute on Windows (needed when locale changes). */
	char exe_path[PATH_MAX];
	const ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
	if (n > 0) {
		exe_path[n] = '\0';
		const pid_t pid = fork();
		if (pid == 0) {
			execl(exe_path, exe_path, (char *)NULL);
			_exit(127);
		}
		/* Parent continues to SDL_Quit / exit below whether fork worked or not. */
	}
#endif
	pvz2_host_pre_sdl_quit();
	SDL_Quit();
	exit(0);
}

/* Persist to config.ini always. Hot-apply when possible; relaunch if the game
 * locale or emulate_iap changed (both need a fresh session). */
static void apply_settings_and_close(void) {
	const pvz2_config_t *live = pvz2_config();
	const bool video_changed =
		g_edit.fullscreen != live->video_fullscreen ||
		(!g_edit.fullscreen &&
		 (g_edit.video_width != live->video_width ||
		  g_edit.video_height != live->video_height));
	const bool fps_changed = g_edit.fps_limit != live->fps_limit;
	const bool display_changed = g_edit.video_display != pvz2_effective_display();

	write_overlay_settings_to_ini();
	if (display_changed) pvz2_config_update_display(g_edit.video_display);

	const int needs_relaunch = pvz2_config_apply_overlay_settings(
		g_edit.fullscreen,
		g_edit.fullscreen ? 0 : g_edit.video_width,
		g_edit.fullscreen ? 0 : g_edit.video_height,
		g_edit.fps_limit,
		g_edit.locale,
		g_edit.cur_symbol,
		g_edit.cur_code,
		g_edit.emulate_iap,
		g_edit.achievement_notifications);

	if (needs_relaunch) {
		relaunch_process();
		return;
	}

	if (fps_changed)
		pvz2_frame_limit_init();
	if (display_changed)
		pvz2_host_apply_display_settings(pvz2_effective_display());
	if (video_changed)
		pvz2_host_apply_video_settings(g_edit.fullscreen,
									   g_edit.video_width,
									   g_edit.video_height);

	g_open = false;
}

/* -------------------------------------------------------------------------
 * Geometry helpers: panel rect
 * ---------------------------------------------------------------------- */
static bool hit(float x, float y, float w, float h, float mx, float my);

static void get_panel_rect(float win_w, float win_h,
					   float &px, float &py, float &pw, float &ph) {
	float s = ui_scale(win_w, win_h);
	if (!g_tex_panel.id) { px = 0; py = 0; pw = win_w; ph = win_h; return; }
	pw = sc((float)g_tex_panel.w, s);
	ph = sc((float)g_tex_panel.h, s);
	px = (win_w - pw) * 0.5f;
	py = (win_h - ph) * 0.5f;
}

static void get_exit_panel_rect(float win_w, float win_h,
							float &px, float &py, float &pw, float &ph) {
	float s = ui_scale(win_w, win_h);
	if (!g_tex_panel_exit.id) { px = 0; py = 0; pw = win_w; ph = win_h; return; }
	pw = sc((float)g_tex_panel_exit.w, s);
	ph = sc((float)g_tex_panel_exit.h, s);
	px = (win_w - pw) * 0.5f;
	py = (win_h - ph) * 0.5f;
}

/* Locale for UI strings: live edit locale while settings are open, else config.ini. */
static const char *ui_locale(void) {
	if (g_open) return g_edit.locale;
	const pvz2_config_t *cfg = pvz2_config();
	return (cfg && cfg->user_locale[0]) ? cfg->user_locale : "en_US";
}

/* Geometry of the two quit-confirm buttons (QUIT left, CANCEL right). */
static void get_quit_btn_rects(float win_w, float win_h,
						   float &qx, float &qy, float &qw, float &qh,
						   float &cx, float &cy, float &cw, float &ch) {
	float s = ui_scale(win_w, win_h);
	float px, py, pw, ph;
	get_exit_panel_rect(win_w, win_h, px, py, pw, ph);
	qw = sc((float)(g_tex_save_up.id ? g_tex_save_up.w : 195), s);
	qh = sc((float)(g_tex_save_up.id ? g_tex_save_up.h : 77), s);
	cw = sc((float)(g_tex_cancel_up.id ? g_tex_cancel_up.w : 195), s);
	ch = sc((float)(g_tex_cancel_up.id ? g_tex_cancel_up.h : 77), s);
	float gap = sc(kQuitBtnGap, s);
	float total = qw + gap + cw;
	qx = px + (pw - total) * 0.5f;
	qy = py + sc(kQuitBtnY, s);
	cx = qx + qw + gap;
	cy = qy;
}

static void dismiss_quit_confirm(void) {
	g_quit_confirm = false;
	g_quit_btn_pressed = false;
	g_cancel_btn_pressed = false;
	/* If the dialog ran during the splash, drop our temporary audio device so
	 * the game can open its own sink without fighting over the output. */
	release_ui_audio_for_game();
}

static void show_quit_confirm(void) {
	refresh_ui_sfx_from_prefs();
	g_quit_confirm = true;
	g_quit_btn_pressed = false;
	g_cancel_btn_pressed = false;
	play_sound(g_snd_exit);
}

static void do_quit_now(void) {
	g_quit_confirm = false;
	g_quit_btn_pressed = false;
	g_cancel_btn_pressed = false;
	/* Settings may still be open with pending edits that were already written
	 * on each click; flush once more so a quit never loses the last change. */
	if (g_open)
		write_overlay_settings_to_ini();
	/* After boot: return to the main loop for a clean session_end. Calling
	 * exit(0) here leaves dynarmic JIT threads alive and often ends in
	 * "Unhandled SIGSEGV" on Linux. During splash/boot there is no loop yet. */
	if (g_host_booted) {
		pvz2_request_app_quit();
		return;
	}
	fflush(stdout);
	pvz2_host_pre_sdl_quit();
	exit(0);
}

static void draw_quit_confirm(float win_w, float win_h) {
	if (!g_tex_panel_exit.id) return;
	const Strings *str = get_strings(ui_locale());
	float s = ui_scale(win_w, win_h);

	float px, py, pw, ph;
	get_exit_panel_rect(win_w, win_h, px, py, pw, ph);
	draw_tex(g_tex_panel_exit, px, py, pw, ph);

	/* Title — same style as settings header */
	if (g_font.ok) {
		float tw = measure_string(g_font, str->quit_title, true, win_w, win_h);
		float tx = px + (pw - tw) * 0.5f;
		float ty = py + sc(kHeaderBaselineY, s);
		draw_string(g_font, str->quit_title, true, tx, ty,
					0xfd/255.f, 0xdc/255.f, 0x67/255.f,
					win_w, win_h, true, sc(5,s), sc(5,s), sc(2,s));
	}

	/* Body text — item font, white with black shadow, two lines */
	if (g_font.ok) {
		float y1 = py + sc(kQuitMsgY, s);
		float y2 = y1 + sc(kQuitMsgLineH, s);
		float w1 = measure_string(g_font, str->quit_msg1, false, win_w, win_h);
		float w2 = measure_string(g_font, str->quit_msg2, false, win_w, win_h);
		draw_string(g_font, str->quit_msg1, false,
					px + (pw - w1) * 0.5f, y1,
					1.0f, 1.0f, 1.0f, win_w, win_h,
					true, sc(2,s), sc(2,s), 0, 0.55f);
		draw_string(g_font, str->quit_msg2, false,
					px + (pw - w2) * 0.5f, y2,
					1.0f, 1.0f, 1.0f, win_w, win_h,
					true, sc(2,s), sc(2,s), 0, 0.55f);
	}

	/* QUIT (btn_save) + CANCEL (btn_cancel) */
	float qx, qy, qw, qh, cx, cy, cw, ch;
	get_quit_btn_rects(win_w, win_h, qx, qy, qw, qh, cx, cy, cw, ch);

	if (g_tex_save_up.id) {
		Tex &qt = g_quit_btn_pressed ? g_tex_save_down : g_tex_save_up;
		draw_tex(qt, qx, qy, qw, qh);
		if (g_font.ok) {
			float cap_h = g_font.scale_item * (float)g_font.ascent_unscaled * s;
			float tw = measure_string(g_font, str->quit_btn, false, win_w, win_h);
			float tx = qx + (qw - tw) * 0.5f;
			float ty = qy + qh * 0.5f + cap_h * 0.35f;
			draw_string(g_font, str->quit_btn, false, tx, ty,
						1.0f, 1.0f, 1.0f, win_w, win_h,
						true, sc(1,s), sc(1,s), 0, 0.50f);
		}
	}
	if (g_tex_cancel_up.id) {
		Tex &ct = g_cancel_btn_pressed ? g_tex_cancel_down : g_tex_cancel_up;
		draw_tex(ct, cx, cy, cw, ch);
		if (g_font.ok) {
			float cap_h = g_font.scale_item * (float)g_font.ascent_unscaled * s;
			float tw = measure_string(g_font, str->cancel_btn, false, win_w, win_h);
			float tx = cx + (cw - tw) * 0.5f;
			float ty = cy + ch * 0.5f + cap_h * 0.35f;
			draw_string(g_font, str->cancel_btn, false, tx, ty,
						1.0f, 1.0f, 1.0f, win_w, win_h,
						true, sc(1,s), sc(1,s), 0, 0.50f);
		}
	}
}

static bool handle_quit_confirm_event(const SDL_Event *e, float win_w, float win_h) {
	float qx, qy, qw, qh, cx, cy, cw, ch;
	get_quit_btn_rects(win_w, win_h, qx, qy, qw, qh, cx, cy, cw, ch);

	if (e->type == SDL_MOUSEMOTION) {
		g_mouse_x = (float)e->motion.x;
		g_mouse_y = (float)e->motion.y;
		return true;
	}
	if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
		g_mouse_x = (float)e->button.x;
		g_mouse_y = (float)e->button.y;
		g_mouse_down = true;
		g_quit_btn_pressed = hit(qx, qy, qw, qh, g_mouse_x, g_mouse_y);
		g_cancel_btn_pressed = hit(cx, cy, cw, ch, g_mouse_x, g_mouse_y);
		return true;
	}
	if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT) {
		float rx = (float)e->button.x, ry = (float)e->button.y;
		g_mouse_down = false;
		if (g_quit_btn_pressed && hit(qx, qy, qw, qh, rx, ry)) {
			g_quit_btn_pressed = false;
			play_sound(g_snd_close);
			do_quit_now();
			return true;
		}
		g_quit_btn_pressed = false;
		if (g_cancel_btn_pressed && hit(cx, cy, cw, ch, rx, ry)) {
			g_cancel_btn_pressed = false;
			play_sound(g_snd_close);
			dismiss_quit_confirm();
			return true;
		}
		g_cancel_btn_pressed = false;
		return true;
	}
	/* Swallow all other input while the dialog is up (including Escape —
	 * main.c toggles via config_screen_request_quit_confirm). */
	return true;
}

static void get_list_rect(float win_w, float win_h,
						  float &lx, float &ly, float &lw, float &lh) {
	float s = ui_scale(win_w, win_h);
	float px, py, pw, ph;
	get_panel_rect(win_w, win_h, px, py, pw, ph);
	lx = px + sc(kListOffX, s);
	ly = py + sc(kListOffY, s);
	lw = sc((float)g_tex_list_bg.w, s);
	lh = sc((float)g_tex_list_bg.h, s);
}

/* -------------------------------------------------------------------------
 * Hit test helpers
 * ---------------------------------------------------------------------- */
static bool hit(float x, float y, float w, float h, float mx, float my) {
	return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

/* Max scroll so the last item's bottom sits on the solid bottom rim (6px),
 * i.e. the list continues flush to the container edge when scrolled down. */
static float list_max_scroll(int total_items, float item_h, float lh, float s) {
	if (total_items <= 0 || item_h <= 0.0f) return 0.0f;
	const float top = sc(kItemStartY, s);
	const float bot = sc(kListSolidEdge, s);
	float max_sc = top + (float)total_items * item_h - (lh - bot);
	return max_sc > 0.0f ? max_sc : 0.0f;
}

/* =========================================================================
 * Scrollbar geometry helper
 * Returns false if scrollbar is not visible (nothing to scroll).
 * out_track_* : full track rect (screen pixels)
 * out_bar_*   : thumb rect (screen pixels)
 * out_travel  : pixels the bar centre can travel
 * out_max_scroll : maximum scroll offset in pixels
 * ========================================================================= */
static bool get_scrollbar_rects(int total_items, float scroll,
                                 float lx, float ly, float lw, float lh,
                                 float dx, float item_h, float s,
                                 float &out_track_x, float &out_track_y,
                                 float &out_track_w, float &out_track_h,
                                 float &out_bar_x,   float &out_bar_y,
                                 float &out_bar_w,   float &out_bar_h,
                                 float &out_travel,  float &out_max_scroll) {
    float max_sc = list_max_scroll(total_items, item_h, lh, s);
    if (max_sc <= 0.0f || !g_tex_scroll_track.id || !g_tex_scroll_bar.id)
        return false;

    float track_w = sc((float)g_tex_scroll_track.w, s);
    float track_x = lx + lw - sc(kScrollTrackOffX, s) - track_w + dx;
    float track_y = ly + sc(kScrollTrackOffY, s);
    float track_h = lh - sc(kScrollTrackOffY * 2, s);

    float bar_w   = sc((float)g_tex_scroll_bar.w, s);
    float bar_h   = sc((float)g_tex_scroll_bar.h, s);
    float ratio   = scroll / max_sc;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    float travel  = track_h - bar_h;
    float bar_x   = track_x + (track_w - bar_w) * 0.5f;
    float bar_y   = track_y + ratio * travel;

    out_track_x = track_x; out_track_y = track_y;
    out_track_w = track_w; out_track_h = track_h;
    out_bar_x   = bar_x;   out_bar_y   = bar_y;
    out_bar_w   = bar_w;   out_bar_h   = bar_h;
    out_travel  = travel;  out_max_scroll = max_sc;
    return true;
}

/* =========================================================================
 * Draw content of one config screen into the panel area
 * dx = horizontal translate offset (pixels, + = right); used for slide anim
 * scroll = vertical scroll offset for the items list
 * screen = which screen's content to draw
 * ========================================================================= */
static void draw_panel_content(Screen screen, float dx,
                               float scroll,
                               float win_w, float win_h,
                               float px, float py, float pw, float ph,
                               bool is_current_screen) {
    const Strings *str = get_strings(g_edit.locale);
    float s = ui_scale(win_w, win_h);

    /* --- Scissor everything to panel interior (18px border at 1080 ref) --- */
    float border = sc(18.0f, s);
    glEnable(GL_SCISSOR_TEST);
    glScissor(
        (GLint)(px + border),
        (GLint)(win_h - (py + ph - border)),
        (GLsizei)(pw - border * 2),
        (GLsizei)(ph - border * 2)
    );

    /* --- close / back button (only for the "current" live screen) --- */
    if (is_current_screen) {
        if (screen == Screen::Main) {
            float bw = sc((float)g_tex_close_up.w, s);
            float bh = sc((float)g_tex_close_up.h, s);
            float bx = px + pw - sc(kCloseBtnFromRight, s) - bw + dx;
            float by = py + sc(kCloseBtnFromTop, s);
            Tex &btn = g_close_pressed ? g_tex_close_down : g_tex_close_up;
            draw_tex(btn, bx, by, bw, bh);
        } else {
            float bw = sc((float)g_tex_back_up.w, s);
            float bh = sc((float)g_tex_back_up.h, s);
            float bx = px + sc(kBackBtnFromLeft, s) + dx;
            float by = py + sc(kBackBtnFromTop, s);
            Tex &btn = g_back_pressed ? g_tex_back_down : g_tex_back_up;
            draw_tex(btn, bx, by, bw, bh);
        }
    }

    /* --- header text --- */
    const char *title = str->title;
    if      (screen == Screen::Resolution) title = str->resolution;
    else if (screen == Screen::Fps)        title = str->fps;
    else if (screen == Screen::Language)   title = str->language;
    else if (screen == Screen::Currency)   title = str->currency;
    else if (screen == Screen::Bindings)   title = str->input_config;
    else if (screen == Screen::GamepadBinds) title = str->gamepad_config;
    else if (screen == Screen::GamepadDevicePick) title = str->gamepad_selected;
    else if (screen == Screen::DisplayPick) title = str->display_config;

    /* BindEdit: use the bind's display name as the title */
    static char s_bind_edit_title[128];
    if (screen == Screen::BindEdit) {
        const pvz2_config_t *cfg = pvz2_config();
        if (g_edit_bind_idx >= 0 && (unsigned)g_edit_bind_idx < cfg->shortcut_count) {
            std::string dn = bind_display_name(cfg->shortcuts[g_edit_bind_idx].name,
                                               g_edit.locale);
            SDL_strlcpy(s_bind_edit_title, dn.c_str(), sizeof(s_bind_edit_title));
        } else {
            SDL_strlcpy(s_bind_edit_title, str->input_config, sizeof(s_bind_edit_title));
        }
        title = s_bind_edit_title;
    }
    if (screen == Screen::GamepadBindPick) {
        SDL_strlcpy(s_bind_edit_title, gamepad_button_label(g_edit_gamepad_btn, str),
                    sizeof(s_bind_edit_title));
        title = s_bind_edit_title;
    }

    if (g_font.ok) {
        /* Shrink the title if it would overlap the close (main) or back (sub) button. */
        const float gap = sc(14.0f, s);
        float left_lim = px + gap + dx;
        float right_lim = px + pw - gap + dx;
        if (screen == Screen::Main && g_tex_close_up.id) {
            float bw = sc((float)g_tex_close_up.w, s);
            right_lim = px + pw - sc(kCloseBtnFromRight, s) - bw - gap + dx;
            /* Keep the title visually centered: mirror the same clearance on the left. */
            left_lim = px + sc(kCloseBtnFromRight, s) + bw + gap + dx;
        } else if (screen != Screen::Main && g_tex_back_up.id) {
            float bw = sc((float)g_tex_back_up.w, s);
            left_lim = px + sc(kBackBtnFromLeft, s) + bw + gap + dx;
            right_lim = px + pw - (sc(kBackBtnFromLeft, s) + bw + gap) + dx;
        }
        float max_w = right_lim - left_lim;
        float title_mul = 1.0f;
        float tw = measure_string(g_font, title, true, win_w, win_h, title_mul);
        if (max_w > 1.0f && tw > max_w)
            title_mul = max_w / tw;
        if (title_mul < 0.45f) title_mul = 0.45f;
        tw = measure_string(g_font, title, true, win_w, win_h, title_mul);
        float tx = px + (pw - tw) * 0.5f + dx;
        float ty = py + sc(kHeaderBaselineY, s);
        draw_string(g_font, title, true, tx, ty, 0xfd/255.f, 0xdc/255.f, 0x67/255.f,
                win_w, win_h, true, sc(5,s) * title_mul, sc(5,s) * title_mul, sc(2,s) * title_mul,
                0.25f, 1.0f, 0, 0, 0, 0, title_mul);
    }

    /* --- list background --- */
    float lx, ly, lw, lh;
    get_list_rect(win_w, win_h, lx, ly, lw, lh);

    /* BindEdit has its own custom layout instead of the item list */
    if (screen == Screen::BindEdit) {
        const pvz2_config_t *cfg = pvz2_config();
        bool valid = (g_edit_bind_idx >= 0 && (unsigned)g_edit_bind_idx < cfg->shortcut_count);

        /* --- txt_bind_input (key capture field) ---
         * Top of txt_bind_input = py + 143px (ref) */
        if (g_tex_bind_input.id) {
            float biw = sc((float)g_tex_bind_input.w, s);
            float bih = sc((float)g_tex_bind_input.h, s);
            float bix = px + (pw - biw) * 0.5f + dx;
            float biy = py + sc(143.0f, s);
            draw_tex(g_tex_bind_input, bix, biy, biw, bih);

            /* Show captured key text centered inside the field */
            if (g_font.ok && g_captured_key[0] != '\0') {
                float cap_h = g_font.scale_item * (float)g_font.ascent_unscaled * s;
                float tw2 = measure_string(g_font, g_captured_key, false, win_w, win_h);
                float tx2 = bix + (biw - tw2) * 0.5f;
                float ty2 = biy + bih * 0.5f + cap_h * 0.35f;
                draw_string(g_font, g_captured_key, false, tx2, ty2,
                            0x48/255.f, 0x3e/255.f, 0x00/255.f, win_w, win_h);
            }
        }

        /* --- img_bind_container ---
         * Top of container = py + 255px (ref), centred in X */
        if (g_tex_bind_container.id) {
            float cw = sc((float)g_tex_bind_container.w, s);
            float ch = sc((float)g_tex_bind_container.h, s);
            float cx = px + (pw - cw) * 0.5f + dx;
            float cy = py + sc(255.0f, s);
            draw_tex(g_tex_bind_container, cx, cy, cw, ch);

            /* img_bind_preview: inset 35px on each side inside container */
            if (g_tex_bind_preview.id) {
                float margin = sc(35.0f, s);
                float pvw = cw - margin * 2.0f;
                float pvh = ch - margin * 2.0f;
                float pvx = cx + margin;
                float pvy = cy + margin;
                /* Keep aspect ratio of the preview image */
                float img_aspect = (g_tex_bind_preview.h > 0)
                    ? (float)g_tex_bind_preview.w / (float)g_tex_bind_preview.h : 1.0f;
                float box_aspect = (pvh > 0) ? pvw / pvh : 1.0f;
                if (img_aspect > box_aspect) {
                    float new_h = pvw / img_aspect;
                    pvy += (pvh - new_h) * 0.5f;
                    pvh = new_h;
                } else {
                    float new_w = pvh * img_aspect;
                    pvx += (pvw - new_w) * 0.5f;
                    pvw = new_w;
                }
                draw_tex(g_tex_bind_preview, pvx, pvy, pvw, pvh);

                /* point_green pulse: two overlapping ripples offset by 75% of the cycle.
                 * Each ripple starts small+opaque and grows+fades over kPulseDurMs. */
                if (g_tex_point_green.id && valid) {
                    float bx_norm = cfg->shortcuts[g_edit_bind_idx].x;
                    float by_norm = cfg->shortcuts[g_edit_bind_idx].y;
                    float base_w  = sc((float)g_tex_point_green.w * 0.5f, s);
                    float base_h  = sc((float)g_tex_point_green.h * 0.5f, s);
                    float cx_pt   = pvx + bx_norm * pvw;
                    float cy_pt   = pvy + by_norm * pvh;

                    Uint32 now = SDL_GetTicks();
                    float t0 = (float)((now - g_pulse_start_ms) % kPulseDurMs) / (float)kPulseDurMs;
                    float t1 = std::fmod(t0 + 0.25f, 1.0f); /* second ripple offset by 25% (born at 75%) */

                    glBindTexture(GL_TEXTURE_2D, g_tex_point_green.id);
                    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

                    auto draw_ripple = [&](float t) {
                        float sc_f = 0.25f + t * 1.75f;   /* 0.25 → 2.0 */
                        float alpha = 1.0f - t;            /* 1.0 → 0.0 */
                        float rw = base_w * sc_f;
                        float rh = base_h * sc_f;
                        glColor4f(1, 1, 1, alpha);
                        glBegin(GL_QUADS);
                        glTexCoord2f(0,0); glVertex2f(cx_pt - rw*0.5f, cy_pt - rh*0.5f);
                        glTexCoord2f(1,0); glVertex2f(cx_pt + rw*0.5f, cy_pt - rh*0.5f);
                        glTexCoord2f(1,1); glVertex2f(cx_pt + rw*0.5f, cy_pt + rh*0.5f);
                        glTexCoord2f(0,1); glVertex2f(cx_pt - rw*0.5f, cy_pt + rh*0.5f);
                        glEnd();
                    };

                    draw_ripple(t0);
                    draw_ripple(t1);
                }
            }

            /* --- Save button: 50px below container bottom --- */
            if (g_tex_save_up.id) {
                float sv_y_top = cy + ch + sc(50.0f, s);
                float svw = sc((float)g_tex_save_up.w, s);
                float svh = sc((float)g_tex_save_up.h, s);
                float svx = px + (pw - svw) * 0.5f + dx;
                Tex &st = g_save_pressed ? g_tex_save_down : g_tex_save_up;
                draw_tex(st, svx, sv_y_top, svw, svh);

                /* "GUARDAR" / "SAVE" label */
                if (g_font.ok) {
                    float cap_h = g_font.scale_item * (float)g_font.ascent_unscaled * s;
                    float tw3 = measure_string(g_font, str->save, false, win_w, win_h);
                    float tx3 = svx + (svw - tw3) * 0.5f;
                    float ty3 = sv_y_top + svh * 0.5f + cap_h * 0.35f;
                    /* 1px shadow */
                    draw_string(g_font, str->save, false,
                                tx3, ty3,
                                1.0f, 1.0f, 1.0f, win_w, win_h,
                                true, sc(1,s), sc(1,s), 0, 0.50f);
                }
            }
        }

        glDisable(GL_SCISSOR_TEST);
        return;
    }

    /* Stamp list_bg into the stencil buffer (opaque texels only) so items
     * cannot draw into the magenta-keyed rounded corners, then paint list_bg. */
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    draw_tex(g_tex_list_bg, lx + dx, ly, lw, lh);
    glDisable(GL_ALPHA_TEST);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_ALWAYS, 0, 0xFF); /* color pass: don't touch stencil */
    draw_tex(g_tex_list_bg, lx + dx, ly, lw, lh);

    /* --- items --- */
    std::vector<Item> items = build_items(screen, str);

    float item_w = sc((float)g_tex_list_item.w, s);
    float item_h = sc((float)g_tex_list_item.h, s);
    float item_x = lx + (lw - item_w) * 0.5f + dx;
    float item_start_y = ly + sc(kItemStartY, s);

    /* Scissor only at the solid 6px rim. Inside the next 10px (shadow),
     * item bg / text / icons keep drawing but ramp alpha → 0 toward the rim.
     * Past fade_end everything stays fully opaque. Scrollbar unchanged.
     * Stencil keeps fragments inside list_bg's rounded shape. */
    float solid = sc(kListSolidEdge, s);
    float fade_end = sc(kListFadeEnd, s);
    float corner_cut = sc(kListCornerCut, s);
    float list_l = lx + dx, list_r = lx + dx + lw;
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glScissor(
        (GLint)(px + border),
        (GLint)(win_h - (ly + lh - solid)),
        (GLsizei)(pw - border * 2),
        (GLsizei)(lh - solid * 2)
    );

    for (size_t i = 0; i < items.size(); ++i) {
        float iy = item_start_y + (float)i * item_h - scroll;
        if (iy + item_h <= ly + solid || iy >= ly + lh - solid) continue;

        draw_list_item_faded(g_tex_list_item, item_x, iy, item_w, item_h,
                             list_l, ly, list_r, ly + lh, solid, fade_end, corner_cut);

        if (g_font.ok) {
            float tx2 = item_x + sc(kItemTextOffX, s);
            float cap_h = g_font.scale_item * (float)g_font.ascent_unscaled * s;
            float line_h = sc(48.0f, s);

            /* Reserve the right-side control (icon or slider track). */
            float icon_w = 0.0f;
            float icon_margin = sc(kArrowFromRight, s);
            if (items[i].is_slider) {
                icon_w = sc((float)g_tex_slider_empty.w, s);
                icon_margin = sc(kSliderFromRight, s);
            } else if (items[i].is_arrow) {
                icon_w = sc((float)g_tex_arrow_up.w, s);
                if (screen == Screen::Main && (int)i == 10 && g_tex_exit_up.id)
                    icon_w = sc((float)g_tex_exit_up.w, s);
            } else if (items[i].is_edit) {
                icon_w = sc((float)g_tex_edit_up.w, s);
            } else {
                icon_w = sc((float)g_tex_chk_on.w, s);
                icon_margin = sc(kChkFromRight, s);
            }
            float text_max_w = item_w - sc(kItemTextOffX, s) - icon_margin - icon_w - sc(10.0f, s);
            if (text_max_w < sc(40.0f, s)) text_max_w = sc(40.0f, s);

            std::string lbl = items[i].label;
            if (!items[i].right_label.empty())
                lbl += ": " + items[i].right_label;

            std::vector<std::string> wrapped =
                wrap_item_label(g_font, lbl.c_str(), text_max_w, win_w, win_h);
            if (wrapped.empty()) wrapped.push_back(lbl);

            const int nlines = (int)wrapped.size();
            const float block_h = cap_h + (float)(nlines - 1) * line_h;
            float baseline0 = iy + (item_h - block_h) * 0.5f + cap_h;
            for (int li = 0; li < nlines; ++li) {
                draw_string(g_font, wrapped[li].c_str(), false, tx2,
                            baseline0 + (float)li * line_h,
                            0x48/255.f, 0x3e/255.f, 0x00/255.f, win_w, win_h);
            }
            /* Text stays fully opaque: fading dark glyphs over cream washes
             * them white. Fade applies only to list_item bg + icons. */
        }

        if (items[i].is_slider) {
            draw_item_slider(item_x, iy, item_w, item_h, s, items[i].slider_t);
        } else if (items[i].is_arrow) {
            bool is_exit_row = (screen == Screen::Main && (int)i == 10 && g_tex_exit_up.id && g_tex_exit_down.id);
            Tex &at = is_exit_row
                ? (((int)i == g_arrow_pressed_idx) ? g_tex_exit_down : g_tex_exit_up)
                : (((int)i == g_arrow_pressed_idx) ? g_tex_arrow_down : g_tex_arrow_up);
            float aw = sc((float)at.w, s);
            float ah = sc((float)at.h, s);
            float ax2 = item_x + item_w - sc(kArrowFromRight, s) - aw;
            float ay2 = iy + (item_h - ah) * 0.5f;
            draw_list_item_faded(at, ax2, ay2, aw, ah,
                                 list_l, ly, list_r, ly + lh, solid, fade_end, corner_cut);
        } else if (items[i].is_edit) {
            Tex &et = ((int)i == g_arrow_pressed_idx) ? g_tex_edit_down : g_tex_edit_up;
            float ew = sc((float)et.w, s);
            float eh = sc((float)et.h, s);
            float ex2 = item_x + item_w - sc(kArrowFromRight, s) - ew;
            float ey2 = iy + (item_h - eh) * 0.5f;
            draw_list_item_faded(et, ex2, ey2, ew, eh,
                                 list_l, ly, list_r, ly + lh, solid, fade_end, corner_cut);
        } else {
            Tex &ct = items[i].checked ? g_tex_chk_on : g_tex_chk_off;
            float cw = sc((float)ct.w, s);
            float ch2 = sc((float)ct.h, s);
            float cx2 = item_x + item_w - sc(kChkFromRight, s) - cw;
            float cy2 = iy + (item_h - ch2) * 0.5f;
            draw_list_item_faded(ct, cx2, cy2, cw, ch2,
                                 list_l, ly, list_r, ly + lh, solid, fade_end, corner_cut);
        }
    }

    /* Restore panel scissor for the scrollbar */
    glDisable(GL_STENCIL_TEST);
    glScissor(
        (GLint)(px + border),
        (GLint)(win_h - (py + ph - border)),
        (GLsizei)(pw - border * 2),
        (GLsizei)(ph - border * 2)
    );

    /* --- scroll track + bar --- */
    int total_items = (int)items.size();
    {
        float track_x, track_y, track_w, track_h;
        float bar_x,   bar_y,   bar_w,   bar_h;
        float travel,  max_scroll;
        if (get_scrollbar_rects(total_items, scroll,
                                lx, ly, lw, lh, dx, item_h, s,
                                track_x, track_y, track_w, track_h,
                                bar_x, bar_y, bar_w, bar_h,
                                travel, max_scroll)) {
            draw_tex(g_tex_scroll_track, track_x, track_y, track_w, track_h);
            draw_tex(g_tex_scroll_bar,   bar_x,   bar_y,   bar_w,   bar_h);
        }
    }

    glDisable(GL_SCISSOR_TEST);
}

/* =========================================================================
 * Draw a single frame of the config screen
 * ========================================================================= */
static void draw(SDL_Window *window) {
	/* Handle deferred open request (from hooks that lack the window pointer) */
	if (g_open_requested && !g_open) {
		g_open_requested = false;
		config_screen_open(window);
	}
	if (!g_open && !g_quit_confirm) return;

	/* Quit-confirm alone still needs resources + window pointer. */
	if (g_quit_confirm && !g_loaded) {
		g_window = window;
		int ww, hh;
		SDL_GL_GetDrawableSize(window, &ww, &hh);
		load_resources((float)ww, (float)hh);
	}

	int win_wi, win_hi;
	SDL_GL_GetDrawableSize(window, &win_wi, &win_hi);
	float win_w = (float)win_wi, win_h = (float)win_hi;

	/* Save GL state */
	GLint prev_program = 0, prev_fbo = 0;
	GLint prev_viewport[4]{};
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	glPushAttrib(GL_ALL_ATTRIB_BITS);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(0);
	glViewport(0, 0, win_wi, win_hi);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix(); glLoadIdentity();
	glOrtho(0, win_w, win_h, 0, -1, 1);  /* top-left origin */
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix(); glLoadIdentity();
	glMatrixMode(GL_TEXTURE);
	glPushMatrix(); glLoadIdentity();

	/* --- dim overlay (once, whether settings and/or quit confirm) --- */
	glDisable(GL_TEXTURE_2D);
	glColor4f(0, 0, 0, 0.5f);
	glBegin(GL_QUADS);
	glVertex2f(0, 0); glVertex2f(win_w, 0);
	glVertex2f(win_w, win_h); glVertex2f(0, win_h);
	glEnd();
	glEnable(GL_TEXTURE_2D);

	if (g_open) {
		/* --- panel background --- */
		float px, py, pw, ph;
		get_panel_rect(win_w, win_h, px, py, pw, ph);
		draw_tex(g_tex_panel, px, py, pw, ph);

		/* --- Update transition timer --- */
		if (g_in_transition) {
			Uint32 now = SDL_GetTicks();
			float elapsed = (float)(now - g_trans_start_ms);
			g_trans_t = elapsed / (float)kTransDurMs;
			if (g_trans_t >= 1.0f) {
				g_trans_t = 1.0f;
				g_in_transition = false;
			}
		}

		/* --- Draw content (with slide if transition active) --- */
		if (g_in_transition) {
			/* ease-out: t_ease = 1 - (1-t)^2 */
			float t = g_trans_t;
			float te = 1.0f - (1.0f - t) * (1.0f - t);

			/* slide direction: forward = current enters from right, prev exits left
			 *                  back    = current enters from left,  prev exits right */
			float sign = g_trans_forward ? 1.0f : -1.0f;
			float dx_curr = sign * pw * (1.0f - te);   /* travels from ±pw → 0       */
			float dx_prev = -sign * pw * te;            /* travels from 0  → ∓pw      */

			/* draw exiting screen first (underneath) */
			draw_panel_content(g_prev_screen, dx_prev, g_prev_scroll,
							   win_w, win_h, px, py, pw, ph, false);
			/* draw entering screen on top */
			draw_panel_content(g_screen, dx_curr, g_scroll_offset,
							   win_w, win_h, px, py, pw, ph, true);
		} else {
			draw_panel_content(g_screen, 0.0f, g_scroll_offset,
							   win_w, win_h, px, py, pw, ph, true);
		}
	}

	/* When quit-confirm opens on top of settings, dim the settings panel too
	 * so only the quit dialog reads as the active window. */
	if (g_open && g_quit_confirm) {
		glDisable(GL_TEXTURE_2D);
		glColor4f(0, 0, 0, 0.5f);
		glBegin(GL_QUADS);
		glVertex2f(0, 0); glVertex2f(win_w, 0);
		glVertex2f(win_w, win_h); glVertex2f(0, win_h);
		glEnd();
		glEnable(GL_TEXTURE_2D);
	}

	/* Quit confirm draws above the settings panel when both are active. */
	if (g_quit_confirm)
		draw_quit_confirm(win_w, win_h);

	/* Restore GL state */
	glMatrixMode(GL_TEXTURE);  glPopMatrix();
	glMatrixMode(GL_MODELVIEW); glPopMatrix();
	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glPopAttrib();
	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	glUseProgram(prev_program);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

/* -------------------------------------------------------------------------
 * Slide transition helpers
 * ---------------------------------------------------------------------- */
static void begin_transition(Screen to, bool forward) {
    /* Save current scroll before leaving this screen */
    g_saved_scroll[(int)g_screen] = g_scroll_offset;

    g_prev_screen    = g_screen;
    g_prev_scroll    = g_scroll_offset;
    g_screen         = to;
    /* Restore previously saved scroll for the destination screen.
     * When going forward into a sub-screen always start at 0. */
    g_scroll_offset  = forward ? 0.0f : g_saved_scroll[(int)to];
    g_trans_forward  = forward;
    g_trans_start_ms = SDL_GetTicks();
    g_in_transition  = true;
    g_trans_t        = 0.0f;
}

/* =========================================================================
 * Input handling
 * ========================================================================= */
static void handle_event(const SDL_Event *e) {
	if (!e) return;
	int win_wi, win_hi;
	SDL_GL_GetDrawableSize(g_window, &win_wi, &win_hi);
	float win_w = (float)win_wi, win_h = (float)win_hi;

	/* Quit confirm is modal: it eats all input while visible. */
	if (g_quit_confirm) {
		handle_quit_confirm_event(e, win_w, win_h);
		return;
	}

	/* Settings must be open for the rest of the handler. */
	if (!g_open) return;

	float px, py, pw, ph;
	get_panel_rect(win_w, win_h, px, py, pw, ph);
	float lx, ly, lw, lh;
	get_list_rect(win_w, win_h, lx, ly, lw, lh);

	const Strings *str = get_strings(g_edit.locale);

	/* Build item list (same logic as draw, just for hit testing) */
	struct RItem { bool is_arrow; bool is_edit; bool is_slider; bool checked; int slider_id;
				   int res_w; int res_h; int res_fs;
				   int fps_val; const char *locale; const char *cur_sym; const char *cur_code; };
	std::vector<RItem> ritems;
	if (g_screen == Screen::Main) {
		/* 0=display 1=res 2=fps 3=lang 4=currency 5=input 6=gamepad
		 * 7=achievements 8=achievement_notify 9=iap 10=exit */
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr}); /* achievements */
		ritems.push_back({false,false,false,g_edit.achievement_notifications!=0,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({false,false,false,g_edit.emulate_iap!=0,-1,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr}); /* exit */
	} else if (g_screen == Screen::Resolution) {
		const std::vector<ResOpt> opts = filtered_res_opts(
			g_edit.fullscreen, g_edit.video_width, g_edit.video_height);
		for (const ResOpt &o : opts)
			ritems.push_back({false,false,false,false,-1,o.w,o.h,o.fs,0,nullptr,nullptr,nullptr});
	} else if (g_screen == Screen::Fps) {
		const std::vector<int> opts = filtered_fps_opts(g_edit.fps_limit);
		for (int v : opts)
			ritems.push_back({false,false,false,false,-1,0,0,0,v,nullptr,nullptr,nullptr});
	} else if (g_screen == Screen::Language) {
		static const char *locs_r[] = {"en_US","es_ES","fr_FR","de_DE","pt_BR","it_IT"};
		for (const char *l : locs_r) ritems.push_back({false,false,false,false,-1,0,0,0,0,l,nullptr,nullptr});
	} else if (g_screen == Screen::Currency) {
		for (int i = 0; i < kCurrencyOptCount; ++i)
			ritems.push_back({false,false,false,false,-1,0,0,0,0,nullptr,
							  kCurrencyOpts[i].sym, kCurrencyOpts[i].code});
	} else if (g_screen == Screen::Bindings) {
		const pvz2_config_t *cfg = pvz2_config();
		for (unsigned i = 0; i < cfg->shortcut_count; ++i)
			ritems.push_back({false,true,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
	} else if (g_screen == Screen::GamepadBinds) {
		ritems.push_back({true, false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr}); /* device */
		ritems.push_back({false,false,true,false,(int)SliderId::StickSpeed,0,0,0,0,nullptr,nullptr,nullptr});
		ritems.push_back({false,false,true,false,(int)SliderId::StickDeadzone,0,0,0,0,nullptr,nullptr,nullptr});
		for (int i = 0; i < kGamepadRemapCount; ++i)
			ritems.push_back({false,true,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
	} else if (g_screen == Screen::GamepadDevicePick) {
		ritems.push_back({false,false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr}); /* Disabled */
		const std::vector<DetectedPad> pads = list_detected_gamepads();
		for (size_t i = 0; i < pads.size(); ++i)
			ritems.push_back({false,false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
	} else if (g_screen == Screen::DisplayPick) {
		const std::vector<DetectedDisplay> displays = list_detected_displays();
		const size_t n = displays.empty() ? 1 : displays.size();
		for (size_t i = 0; i < n; ++i)
			ritems.push_back({false,false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
	} else if (g_screen == Screen::GamepadBindPick) {
		/* None + one row per shortcut */
		ritems.push_back({false,false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
		const pvz2_config_t *cfg = pvz2_config();
		for (unsigned i = 0; i < cfg->shortcut_count; ++i)
			ritems.push_back({false,false,false,false,-1,0,0,0,0,nullptr,nullptr,nullptr});
	}
	float item_w = sc((float)g_tex_list_item.w, ui_scale(win_w, win_h));
	float item_h = sc((float)g_tex_list_item.h, ui_scale(win_w, win_h));
	float item_x = lx + (lw - item_w) * 0.5f;
	float item_start_y = ly + sc(kItemStartY, ui_scale(win_w, win_h));

	/* Close / back button rects */
	float close_x=0,close_y=0,close_w=0,close_h=0;
	float back_x=0,back_y=0,back_w=0,back_h=0;
	if (g_screen == Screen::Main) {
		close_w = sc((float)g_tex_close_up.w, ui_scale(win_w,win_h));
		close_h = sc((float)g_tex_close_up.h, ui_scale(win_w,win_h));
		close_x = px+pw-sc(kCloseBtnFromRight,ui_scale(win_w,win_h))-close_w;
		close_y = py+sc(kCloseBtnFromTop,ui_scale(win_w,win_h));
	} else {
		back_w = sc((float)g_tex_back_up.w, ui_scale(win_w,win_h));
		back_h = sc((float)g_tex_back_up.h, ui_scale(win_w,win_h));
		back_x = px+sc(kBackBtnFromLeft,ui_scale(win_w,win_h));
		back_y = py+sc(kBackBtnFromTop,ui_scale(win_w,win_h));
	}

	if (e->type == SDL_MOUSEMOTION) {
		g_mouse_x = (float)e->motion.x;
		g_mouse_y = (float)e->motion.y;

		if (g_slider_drag != (int)SliderId::None) {
			float s2 = ui_scale(win_w, win_h);
			/* Find the slider row currently being dragged to map mouse → value. */
			for (size_t i = 0; i < ritems.size(); ++i) {
				if (!ritems[i].is_slider || ritems[i].slider_id != g_slider_drag) continue;
				float iy = item_start_y + (float)i * item_h - g_scroll_offset;
				float stx, sty, stw, sth;
				slider_track_rect(item_x, iy, item_w, item_h, s2, stx, sty, stw, sth);
				apply_slider_t(g_slider_drag, slider_t_from_mouse_x(g_mouse_x, stx, stw), false);
				break;
			}
		} else if (g_scrollbar_dragging) {
			float ih = sc((float)g_tex_list_item.h, ui_scale(win_w, win_h));
			float track_x, track_y, track_w, track_h;
			float bar_x,   bar_y,   bar_w,   bar_h;
			float travel = 0,  max_scroll = 0;
			if (get_scrollbar_rects((int)ritems.size(), g_scroll_offset,
												   lx, ly, lw, lh, 0, ih,
												   ui_scale(win_w, win_h),
												   track_x, track_y, track_w, track_h,
												   bar_x,   bar_y,   bar_w,   bar_h,
												   travel,  max_scroll) && travel > 0) {
				float delta_mouse = g_mouse_y - g_scrollbar_drag_mouse_y0;
				g_scroll_offset = g_scrollbar_drag_scroll0 + delta_mouse * (max_scroll / travel);
				if (g_scroll_offset < 0) g_scroll_offset = 0;
				if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;
			}
		} else if (g_list_drag_armed || g_list_dragging) {
			/* Drag content with the pointer: finger down → list follows down. */
			float dy = g_mouse_y - g_list_drag_mouse_y0;
			if (!g_list_dragging && fabsf(dy) >= kListDragThreshold) {
				g_list_dragging = true;
				g_arrow_pressed_idx = -1; /* cancel pressed visual once scrolling */
			}
			if (g_list_dragging) {
				float ih = sc((float)g_tex_list_item.h, ui_scale(win_w, win_h));
				float lh2 = sc((float)g_tex_list_bg.h, ui_scale(win_w, win_h));
				float max_scroll = list_max_scroll((int)ritems.size(), ih, lh2,
												   ui_scale(win_w, win_h));
				g_scroll_offset = g_list_drag_scroll0 - dy;
				if (g_scroll_offset < 0) g_scroll_offset = 0;
				if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;
			}
		}
	} else if (e->type == SDL_KEYDOWN) {
		/* Key capture for BindEdit screen */
		if (g_screen == Screen::BindEdit) {
			SDL_Keycode kc = e->key.keysym.sym;
			/* Ignore Escape and F11 */
			if (kc == SDLK_ESCAPE || kc == SDLK_F11) return;
			/* Use scancode name: guaranteed round-trip with SDL_GetScancodeFromName.
			 * Verify it resolves back before accepting; if not, keep current value. */
			SDL_Scancode sc = e->key.keysym.scancode;
			const char *scname = SDL_GetScancodeName(sc);
			if (scname && scname[0] != '\0' &&
				SDL_GetScancodeFromName(scname) != SDL_SCANCODE_UNKNOWN) {
				SDL_strlcpy(g_captured_key, scname, sizeof(g_captured_key));
				/* Convert to uppercase for display and storage */
				for (char *p = g_captured_key; *p; ++p)
					*p = (char)std::toupper((unsigned char)*p);
			}
		}
	} else if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
		g_mouse_x = (float)e->button.x;
		g_mouse_y = (float)e->button.y;
		g_mouse_down = true;

		if (g_screen == Screen::Main && hit(close_x,close_y,close_w,close_h,g_mouse_x,g_mouse_y))
			g_close_pressed = true;
		else if (g_screen != Screen::Main && hit(back_x,back_y,back_w,back_h,g_mouse_x,g_mouse_y))
			g_back_pressed = true;
		else if (g_screen == Screen::BindEdit && g_tex_bind_container.id) {
			/* Save button hit detection */
			float s2 = ui_scale(win_w, win_h);
			float cw2 = sc((float)g_tex_bind_container.w, s2);
			float ch2 = sc((float)g_tex_bind_container.h, s2);
			float cx2 = px + (pw - cw2) * 0.5f;
			float cy2 = py + sc(255.0f, s2);
			if (g_tex_save_up.id) {
				float sv_y = cy2 + ch2 + sc(50.0f, s2);
				float svw2 = sc((float)g_tex_save_up.w, s2);
				float svh2 = sc((float)g_tex_save_up.h, s2);
				float svx2 = px + (pw - svw2) * 0.5f;
				if (hit(svx2, sv_y, svw2, svh2, g_mouse_x, g_mouse_y))
					g_save_pressed = true;
			}
		} else {
			/* Check if click is on the scrollbar track first */
			float ih = sc((float)g_tex_list_item.h, ui_scale(win_w, win_h));
			float track_x, track_y, track_w, track_h;
			float bar_x,   bar_y,   bar_w,   bar_h;
			float travel,  max_scroll;
			bool on_scrollbar = false;
			if (get_scrollbar_rects((int)ritems.size(), g_scroll_offset,
									 lx, ly, lw, lh, 0, ih, ui_scale(win_w, win_h),
									 track_x, track_y, track_w, track_h,
									 bar_x,   bar_y,   bar_w,   bar_h,
									 travel,  max_scroll)) {
				if (hit(track_x, track_y, track_w, track_h, g_mouse_x, g_mouse_y)) {
					on_scrollbar = true;
					g_scrollbar_dragging      = true;
					g_scrollbar_drag_mouse_y0 = g_mouse_y;
					g_scrollbar_drag_scroll0  = g_scroll_offset;
					/* If click is directly on the bar we drag from current pos;
					 * if on the track outside the bar, jump scroll to that position. */
					if (!hit(bar_x, bar_y, bar_w, bar_h, g_mouse_x, g_mouse_y) && travel > 0) {
						float ratio = (g_mouse_y - track_y - bar_h * 0.5f) / travel;
						if (ratio < 0) ratio = 0;
						if (ratio > 1) ratio = 1;
						g_scroll_offset = ratio * max_scroll;
						g_scrollbar_drag_scroll0  = g_scroll_offset;
						g_scrollbar_drag_mouse_y0 = g_mouse_y;
					}
				}
			}

			if (!on_scrollbar) {
				/* Prefer starting a slider scrub over list-drag / item press. */
				bool on_slider = false;
				if (g_screen == Screen::GamepadBinds) {
					float s2 = ui_scale(win_w, win_h);
					for (size_t i = 0; i < ritems.size(); ++i) {
						if (!ritems[i].is_slider) continue;
						float iy = item_start_y + (float)i * item_h - g_scroll_offset;
						float stx, sty, stw, sth;
						slider_track_rect(item_x, iy, item_w, item_h, s2, stx, sty, stw, sth);
						/* Generous hit: track plus thumb radius. */
						float pad = sc((float)g_tex_slider_thumb.h, s2) * 0.5f;
						if (hit(stx - pad, sty - pad, stw + pad * 2.0f, sth + pad * 2.0f,
								g_mouse_x, g_mouse_y)) {
							g_slider_drag = ritems[i].slider_id;
							apply_slider_t(g_slider_drag, slider_t_from_mouse_x(g_mouse_x, stx, stw), false);
							on_slider = true;
							break;
						}
					}
				}

				/* Arm touch-like list drag when pressing inside the list area. */
				g_list_drag_armed = false;
				g_list_dragging = false;
				if (!on_slider && hit(lx, ly, lw, lh, g_mouse_x, g_mouse_y)) {
					g_list_drag_armed = true;
					g_list_drag_mouse_y0 = g_mouse_y;
					g_list_drag_scroll0 = g_scroll_offset;
				}
				/* Check if an arrow/edit item was pressed */
				g_arrow_pressed_idx = -1;
				if (!on_slider) {
					for (size_t i = 0; i < ritems.size(); ++i) {
						if (!ritems[i].is_arrow && !ritems[i].is_edit) continue;
						float iy = item_start_y + (float)i * item_h - g_scroll_offset;
						if (hit(item_x, iy, item_w, item_h, g_mouse_x, g_mouse_y)) {
							g_arrow_pressed_idx = (int)i;
							break;
						}
					}
				}
			}
		}

	} else if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT) {
		float rx = (float)e->button.x, ry = (float)e->button.y;
		g_mouse_down = false;
		g_arrow_pressed_idx = -1;

		/* End scrollbar / list / slider drag – if we were dragging, consume the event */
		bool was_slider = (g_slider_drag != (int)SliderId::None);
		bool was_dragging = g_scrollbar_dragging || g_list_dragging || was_slider;
		if (was_slider) {
			/* Final persist of the live value when the thumb is released. */
			const pvz2_config_t *cfg = pvz2_config();
			if (g_slider_drag == (int)SliderId::StickSpeed)
				pvz2_config_update_gamepad_stick_param(0, cfg->gamepad_stick_speed, 1);
			else if (g_slider_drag == (int)SliderId::StickDeadzone)
				pvz2_config_update_gamepad_stick_param(1, cfg->gamepad_stick_deadzone, 1);
			play_sound(g_snd_slider);
		}
		g_scrollbar_dragging = false;
		g_list_dragging = false;
		g_list_drag_armed = false;
		g_slider_drag = (int)SliderId::None;
		if (was_dragging) { g_save_pressed = false; return; }

		/* Ignore clicks while transition is playing */
		if (g_in_transition) { g_save_pressed = false; return; }

		/* Close button */
		if (g_close_pressed && hit(close_x,close_y,close_w,close_h,rx,ry)) {
			g_close_pressed = false;
			play_sound(g_snd_close);
			if (g_edit.differs_from_live()) {
				apply_settings_and_close();
			} else {
				g_open = false;
			}
			return;
		}
		g_close_pressed = false;

		/* Back button */
		if (g_back_pressed && hit(back_x,back_y,back_w,back_h,rx,ry)) {
			g_back_pressed = false;
			play_sound(g_snd_back);
			if (g_screen == Screen::BindEdit) {
				/* Cancel: return to Bindings without saving */
				begin_transition(Screen::Bindings, false);
			} else if (g_screen == Screen::GamepadBindPick ||
			           g_screen == Screen::GamepadDevicePick) {
				begin_transition(Screen::GamepadBinds, false);
			} else if (g_screen == Screen::GamepadBinds || g_screen == Screen::Bindings) {
				begin_transition(Screen::Main, false);
			} else {
				begin_transition(Screen::Main, false);
			}
			return;
		}
		g_back_pressed = false;

		/* Save button (BindEdit only) */
		if (g_screen == Screen::BindEdit && g_save_pressed && g_tex_bind_container.id) {
			g_save_pressed = false;
			if (g_captured_key[0] != '\0' && g_edit_bind_idx >= 0) {
						play_sound(g_snd_confirm);
						pvz2_config_update_bind_key((unsigned)g_edit_bind_idx, g_captured_key);
						pvz2_reload_shortcuts();
					}
			begin_transition(Screen::Bindings, false);
			return;
		}
		g_save_pressed = false;

		/* BindEdit: no item list clicks */
		if (g_screen == Screen::BindEdit) return;

		/* Item click */
		/* Block item clicks when mouse is over the scrollbar */
		{
			float ih2 = sc((float)g_tex_list_item.h, ui_scale(win_w, win_h));
			float track_x, track_y, track_w, track_h;
			float bar_x,   bar_y,   bar_w,   bar_h;
			float travel,  max_scroll;
			if (get_scrollbar_rects((int)ritems.size(), g_scroll_offset,
									 lx, ly, lw, lh, 0, ih2, ui_scale(win_w, win_h),
									 track_x, track_y, track_w, track_h,
									 bar_x,   bar_y,   bar_w,   bar_h,
									 travel,  max_scroll)) {
				if (hit(track_x, track_y, track_w, track_h, rx, ry)) return;
			}
		}
		for (size_t i = 0; i < ritems.size(); ++i) {
			float iy = item_start_y + (float)i * item_h - g_scroll_offset;
			/* skip items outside the solid rim; fade band remains clickable */
			float sm = sc(kListSolidEdge, ui_scale(win_w, win_h));
			if (iy + item_h <= ly + sm || iy >= ly + lh - sm) continue;
			if (!hit(item_x, iy, item_w, item_h, rx, ry)) continue;

			if (ritems[i].is_arrow) {
				if (g_screen == Screen::Main) {
					if (i == 10) {
						/* Exit: no menu click SFX — dialog open plays exit_click. */
						show_quit_confirm();
					} else if (i == 7) {
						/* View Achievements — open SFX is exit_click (same as quit). */
						achievements_screen_request_open();
					} else {
						play_sound(g_snd_next);
						Screen to = Screen::Main;
						if      (i == 0) to = Screen::DisplayPick;
						else if (i == 1) to = Screen::Resolution;
						else if (i == 2) to = Screen::Fps;
						else if (i == 3) to = Screen::Language;
						else if (i == 4) to = Screen::Currency;
						else if (i == 5) to = Screen::Bindings;
						else if (i == 6) to = Screen::GamepadBinds;
						if (to != Screen::Main) begin_transition(to, true);
					}
				} else if (g_screen == Screen::GamepadBinds) {
					/* Row 0: selected gamepad picker */
					if (i == 0) {
						play_sound(g_snd_next);
						begin_transition(Screen::GamepadDevicePick, true);
					}
				} else {
					play_sound(g_snd_next);
				}
			} else if (ritems[i].is_edit) {
				play_sound(g_snd_next);
				if (g_screen == Screen::Bindings) {
					g_edit_bind_idx = (int)i;
							/* Pre-fill with current key */
							const pvz2_config_t *cfg = pvz2_config();
							SDL_strlcpy(g_captured_key, cfg->shortcuts[i].key, sizeof(g_captured_key));
							g_pulse_start_ms = SDL_GetTicks();
							begin_transition(Screen::BindEdit, true);
				} else if (g_screen == Screen::GamepadBinds) {
					/* 0=device, 1=speed, 2=deadzone, 3..=remap buttons */
					const int bi = (int)i - 3;
					if (bi >= 0 && bi < kGamepadRemapCount) {
						SDL_strlcpy(g_edit_gamepad_btn, kGamepadRemapButtons[bi],
						            sizeof(g_edit_gamepad_btn));
						begin_transition(Screen::GamepadBindPick, true);
					}
				}
			} else {
				play_sound(g_snd_check);
				if (g_screen == Screen::Main) {
					if (i == 8) {
						g_edit.achievement_notifications = !g_edit.achievement_notifications;
						write_overlay_settings_to_ini();
						/* Apply live immediately — no relaunch needed. Keep other
						 * overlay fields from live so pending locale/IAP edits are
						 * not applied early. */
						const pvz2_config_t *live = pvz2_config();
						pvz2_config_apply_overlay_settings(
							live->video_fullscreen,
							live->video_width, live->video_height,
							live->fps_limit,
							live->user_locale,
							live->currency_symbol,
							live->currency_code,
							live->emulate_iap,
							g_edit.achievement_notifications);
					} else if (i == 9) {
						g_edit.emulate_iap = !g_edit.emulate_iap;
						write_overlay_settings_to_ini();
					}
				} else if (g_screen == Screen::Resolution) {
					const std::vector<ResOpt> ropts = filtered_res_opts(
						g_edit.fullscreen, g_edit.video_width, g_edit.video_height);
					if (i < ropts.size()) {
						g_edit.fullscreen   = ropts[i].fs;
						g_edit.video_width  = ropts[i].w;
						g_edit.video_height = ropts[i].h;
						write_overlay_settings_to_ini();
					}
				} else if (g_screen == Screen::Fps) {
					const std::vector<int> fopts = filtered_fps_opts(g_edit.fps_limit);
					if (i < fopts.size()) {
						g_edit.fps_limit = fopts[i];
						write_overlay_settings_to_ini();
					}
				} else if (g_screen == Screen::Language) {
					static const char *locs_h[] = {"en_US","es_ES","fr_FR","de_DE","pt_BR","it_IT"};
					if ((size_t)i < sizeof(locs_h)/sizeof(locs_h[0])) {
						SDL_strlcpy(g_edit.locale, locs_h[i], sizeof(g_edit.locale));
						write_overlay_settings_to_ini();
					}
				} else if (g_screen == Screen::Currency) {
					if (i < kCurrencyOptCount) {
						SDL_strlcpy(g_edit.cur_symbol, kCurrencyOpts[i].sym,  sizeof(g_edit.cur_symbol));
						SDL_strlcpy(g_edit.cur_code,   kCurrencyOpts[i].code, sizeof(g_edit.cur_code));
						write_overlay_settings_to_ini();
					}
				} else if (g_screen == Screen::GamepadDevicePick) {
					if (i == 0) {
						pvz2_config_update_gamepad_device(0, 0);
					} else {
						const std::vector<DetectedPad> pads = list_detected_gamepads();
						const size_t di = (size_t)(i - 1);
						if (di < pads.size())
							pvz2_config_update_gamepad_device(1, pads[di].index);
					}
					pvz2_gamepad_reload();
				} else if (g_screen == Screen::DisplayPick) {
					const std::vector<DetectedDisplay> displays = list_detected_displays();
					if (i < displays.size()) {
						g_edit.video_display = displays[i].index;
						write_overlay_settings_to_ini();
					}
				} else if (g_screen == Screen::GamepadBindPick) {
					if (g_edit_gamepad_btn[0] == '\0') break;
					if (i == 0) {
						pvz2_config_update_gamepad_bind(g_edit_gamepad_btn, "NULL");
					} else {
						const pvz2_config_t *cfg = pvz2_config();
						const unsigned si = (unsigned)(i - 1);
						if (si < cfg->shortcut_count) {
							char full[72];
							SDL_snprintf(full, sizeof(full), "bind_%s", cfg->shortcuts[si].name);
							pvz2_config_update_gamepad_bind(g_edit_gamepad_btn, full);
						}
					}
					pvz2_gamepad_reload();
				}
			}
			break;
		}

	} else if (e->type == SDL_MOUSEWHEEL) {
		float item_h2 = sc((float)g_tex_list_item.h, ui_scale(win_w, win_h));
		float lh2 = sc((float)g_tex_list_bg.h, ui_scale(win_w, win_h));
		int total = (int)ritems.size();
		float max_scroll = list_max_scroll(total, item_h2, lh2, ui_scale(win_w, win_h));
		g_scroll_offset -= e->wheel.y * item_h2;
		if (g_scroll_offset < 0) g_scroll_offset = 0;
		if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;
	}

	(void)str;
}

} // anonymous namespace

/* =========================================================================
 * Public C API
 * ========================================================================= */
extern "C" {

void config_screen_open(SDL_Window *window) {
	if (g_open) return;
	g_window = window;

	refresh_ui_sfx_from_prefs();

	int win_w, win_h;
	SDL_GL_GetDrawableSize(window, &win_w, &win_h);
	load_resources((float)win_w, (float)win_h);

	g_edit.load_from_live();
	g_screen = Screen::Main;
	g_scroll_offset = 0;
	g_scrollbar_dragging = false;
	g_list_drag_armed = false;
	g_list_dragging = false;
	memset(g_saved_scroll, 0, sizeof(g_saved_scroll));
	g_close_pressed = false;
	g_back_pressed  = false;
	g_save_pressed  = false;
	g_edit_gamepad_btn[0] = '\0';
	g_slider_drag = (int)SliderId::None;
	g_quit_btn_pressed = false;
	g_cancel_btn_pressed = false;
	g_in_transition = false;
	g_trans_t       = 1.0f;
	g_open = true;
}

void config_screen_request_open(void) {
    if (!g_open) g_open_requested = true;
}

void config_screen_request_quit_confirm(void) {
	if (achievements_screen_is_open()) {
		achievements_screen_request_close();
		return;
	}
	if (g_quit_confirm) {
		dismiss_quit_confirm();
		return;
	}
	show_quit_confirm();
}

void config_screen_notify_booted(void) {
	g_host_booted = true;
}

void config_screen_shutdown(void) {
	close_ui_audio_device();
}

int config_screen_is_open(void) {
	return (g_open || g_open_requested || g_quit_confirm) ? 1 : 0;
}

void config_screen_handle_event(const SDL_Event *e) {
	/* Achievements overlay is modal over settings: swallow all config input. */
	if (achievements_screen_is_open()) return;
	if (!g_open && !g_quit_confirm) return;
	/* Quit-confirm alone may not have a window yet; wait until draw assigns one. */
	if (!g_window) return;
	handle_event(e);
}

void config_screen_draw(SDL_Window *window) {
	/* Hide settings while the achievements overlay is up; g_open stays true so
	 * closing achievements restores this screen as it was. */
	if (achievements_screen_is_open()) return;
	if ((!g_open && !g_open_requested && !g_quit_confirm) || !window) return;
	draw(window);
}

void config_screen_play_exit_sound(void) {
	play_sound(g_snd_exit);
}

void config_screen_play_achievement_unlock_sound(void) {
	refresh_ui_sfx_from_prefs();
	play_sound(g_snd_achieve_unlock);
}

} // extern "C"
