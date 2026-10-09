// SPDX-License-Identifier: MIT
//
// moonlight-menu: the settings screen for the Moonlight pak.
//
// moonlight-user.conf is a shell file on an SD card, which is a good place to
// keep settings and a hopeless place to edit them on a handheld: no keyboard,
// no terminal, and one wrong quote in the file and the pak stops starting.
// This is the same settings, reached with a d-pad, saved back in the same
// format the launcher sources.
//
// It also owns the one setting that cannot be guessed. The MLP1's four face
// buttons are arranged the way a Nintendo pad's are, games draw their prompts
// the way an Xbox pad's are, and which of the two should win turns out to be
// personal -- and to differ from one game to the next. So each printed button
// can be pointed at any button the host knows, and the choice is saved.
//
//   moonlight-menu --conf FILE --out FILE [--font PATH] [--timeout SECONDS]
//   moonlight-menu --selftest --conf FILE --out FILE   (no SDL, for CI)
//
// Writes the chosen action -- stream, pair, list, diag, probe or none -- to
// the --out file for the launcher to read, and exits 0.
#define _POSIX_C_SOURCE 200809L   // access, setenv with -std=c11

#include <SDL.h>
#include <SDL_ttf.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define STR 64
#define MAX_ROWS 24

/* ---------------------------------------------------------------- settings */

typedef struct {
    char host[STR], app[STR], mode[16];
    char width[8], height[8], fps[8], bitrate[8], codec[8], packetsize[8];
    char layout[16], quitcombo[STR], extra[STR];
    /* How the four face buttons are wired to what SDL reports. Empty means
       work it out from the device; see pad_wiring_init. */
    char wiring[16];
    /* What the host receives for each button as it is printed on the device:
       map[0] is the button printed A (on the right), map[1] is B (at the
       bottom), map[2] is X (on top), map[3] is Y (on the left). */
    int map[4];
} Settings;

static const char *FACE_NAMES[4] = {"A", "B", "X", "Y"};

/* PAD_LAYOUT names the two arrangements; PAD_MAP is the explicit table the
   settings screen writes when the user points the buttons somewhere else. */
static void apply_layout(Settings *s, const char *layout);

static void defaults(Settings *s) {
    memset(s, 0, sizeof *s);
    snprintf(s->app, sizeof s->app, "Steam");
    snprintf(s->mode, sizeof s->mode, "stream");
    snprintf(s->width, sizeof s->width, "960");
    snprintf(s->height, sizeof s->height, "720");
    snprintf(s->fps, sizeof s->fps, "30");
    snprintf(s->bitrate, sizeof s->bitrate, "5000");
    snprintf(s->codec, sizeof s->codec, "h264");
    snprintf(s->packetsize, sizeof s->packetsize, "1024");
    /* xbox, because what gets streamed is a PC game and a PC game draws its
       prompts for an Xbox pad: the button that confirms is the one at the
       bottom, which on this device is the one printed B. The map has to move
       with it, or the four rows below would show a table the preset does not
       actually mean. */
    snprintf(s->layout, sizeof s->layout, "xbox");
    apply_layout(s, s->layout);
    snprintf(s->quitcombo, sizeof s->quitcombo, "back,start");
}

static void copy_str(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

static int face_index(const char *name) {
    for (int i = 0; i < 4; ++i)
        if (strcasecmp(name, FACE_NAMES[i]) == 0) return i;
    return -1;
}

static void apply_layout(Settings *s, const char *layout) {
    for (int i = 0; i < 4; ++i) s->map[i] = i;
    if (strcasecmp(layout, "xbox") == 0 || strcasecmp(layout, "swap") == 0) {
        s->map[0] = 1; s->map[1] = 0; s->map[2] = 3; s->map[3] = 2;
    }
}

static void apply_map_spec(Settings *s, const char *spec) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        int from = face_index(tok), to = face_index(eq + 1);
        if (from >= 0 && to >= 0) s->map[from] = to;
    }
}

static void set_kv(Settings *s, const char *key, const char *val) {
    if (!val) val = "";
    if (strcmp(key, "HOST") == 0) copy_str(s->host, sizeof s->host, val);
    else if (strcmp(key, "APP") == 0) copy_str(s->app, sizeof s->app, val);
    else if (strcmp(key, "MODE") == 0) {
        /* "menu" is how the pak opens, not something it then does. Reading it
           as an action would send the launcher straight back into this screen,
           so it arrives here as the one thing worth doing by default. */
        copy_str(s->mode, sizeof s->mode, strcasecmp(val, "menu") == 0 ? "stream" : val);
    }
    else if (strcmp(key, "WIDTH") == 0) copy_str(s->width, sizeof s->width, val);
    else if (strcmp(key, "HEIGHT") == 0) copy_str(s->height, sizeof s->height, val);
    else if (strcmp(key, "FPS") == 0) copy_str(s->fps, sizeof s->fps, val);
    else if (strcmp(key, "BITRATE") == 0) copy_str(s->bitrate, sizeof s->bitrate, val);
    else if (strcmp(key, "CODEC") == 0) copy_str(s->codec, sizeof s->codec, val);
    else if (strcmp(key, "PACKETSIZE") == 0) copy_str(s->packetsize, sizeof s->packetsize, val);
    else if (strcmp(key, "PAD_LAYOUT") == 0) {
        copy_str(s->layout, sizeof s->layout, val);
        apply_layout(s, val);
    } else if (strcmp(key, "PAD_MAP") == 0) {
        apply_map_spec(s, val);
        if (strlen(val) > 0) copy_str(s->layout, sizeof s->layout, "custom");
    } else if (strcmp(key, "QUIT_COMBO") == 0) copy_str(s->quitcombo, sizeof s->quitcombo, val);
    else if (strcmp(key, "PAD_WIRING") == 0) copy_str(s->wiring, sizeof s->wiring, val);
    else if (strcmp(key, "EXTRA") == 0) copy_str(s->extra, sizeof s->extra, val);
}

static void load_conf(Settings *s, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char *key = line, *val = eq + 1;
        *eq = 0;
        while (*key == ' ' || *key == '\t') ++key;
        char *ke = key + strlen(key);
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        if (key[0] == '#') continue;
        while (*val == ' ' || *val == '\t') ++val;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        if (ve > val && (ve[-1] == '"' || ve[-1] == '\'')) *--ve = 0;
        if (val[0] == '"' || val[0] == '\'') ++val;
        set_kv(s, key, val);
    }
    fclose(f);
}

static int save_conf(const Settings *s, const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    fprintf(f,
        "# Moonlight pak settings, saved by the pak's own settings screen.\n"
        "# Sourced by launch.sh, so these are plain shell assignments.\n"
        "# Hand edits still work; the screen rewrites this file, keeping your\n"
        "# values and dropping any comments you add here.\n"
        "\n"
        "HOST=\"%s\"\n"
        "APP=\"%s\"\n"
        "\n"
        "# Stays \"menu\": this screen is how the pak opens, and what to do once\n"
        "# it closes travels to launch.sh in its own file. Set MODE to stream by\n"
        "# hand and the pak skips this screen entirely.\n"
        "MODE=\"menu\"\n"
        "\n"
        "# The MLP1's panel is 960x720 and 4:3 -- asking for exactly that means\n"
        "# nothing is scaled on the way to the screen.\n"
        "WIDTH=\"%s\"\n"
        "HEIGHT=\"%s\"\n"
        "\n"
        "# There is no hardware decoder in this build, so 30 is what four\n"
        "# Cortex-A55 cores keep up with at 720p.\n"
        "FPS=\"%s\"\n"
        "\n"
        "# Kbps. This, not the resolution, is what the network pays for.\n"
        "BITRATE=\"%s\"\n"
        "CODEC=\"%s\"\n"
        "\n"
        "# Under the 1500-byte MTU: a video packet that gets fragmented and then\n"
        "# partly lost costs the whole frame.\n"
        "PACKETSIZE=\"%s\"\n"
        "\n"
        "# Face buttons. xbox = the position a game draws wins (A<->B, X<->Y),\n"
        "# so the button printed B, which sits at the bottom, is the one a game\n"
        "# confirms with. nintendo = the letter printed on the button wins, for\n"
        "# a host that flips it back. custom = the PAD_MAP below decides, one\n"
        "# printed button at a time.\n"
        "PAD_LAYOUT=\"%s\"\n",
        s->host, s->app, s->width, s->height, s->fps, s->bitrate,
        s->codec, s->packetsize, s->layout);

    if (strcasecmp(s->layout, "custom") == 0) {
        fprintf(f, "PAD_MAP=\"a=%s,b=%s,x=%s,y=%s\"\n",
                FACE_NAMES[s->map[0]], FACE_NAMES[s->map[1]],
                FACE_NAMES[s->map[2]], FACE_NAMES[s->map[3]]);
    } else {
        fprintf(f, "PAD_MAP=\"\"\n");
    }

    fprintf(f,
        "\n# How the four face buttons are wired to what SDL reports. Leave it\n"
        "# empty and it is worked out from the device. Set it to \"labels\" if a\n"
        "# pad ever turns out to be numbered the way it is printed.\n"
        "PAD_WIRING=\"%s\"\n"
        "\n# Held together, these end a stream. Names are the ones the key probe\n"
        "# prints (MODE=probe), and they are unaffected by PAD_LAYOUT.\n"
        "QUIT_COMBO=\"%s\"\n"
        "\n"
        "EXTRA=\"%s\"\n",
        s->wiring, s->quitcombo, s->extra);

    if (fclose(f) != 0) return -1;
    return rename(tmp, path);
}

/* -------------------------------------------------------------------- rows */

enum {
    R_ACTION, R_HOST, R_APP, R_RES, R_FPS, R_BITRATE, R_CODEC, R_PACKET,
    R_LAYOUT, R_MAPA, R_MAPB, R_MAPX, R_MAPY, R_QUIT, R_START, R_SAVE, R_COUNT
};

static const char *OPTS_ACTION[] = {"stream", "pair", "list", "diag", "probe"};
static const char *OPTS_RES[] = {"960x720", "1280x720", "1024x768", "1920x1080", "960x540"};
static const char *OPTS_FPS[] = {"30", "60"};
static const char *OPTS_BITRATE[] = {"3000", "5000", "8000", "10000", "15000", "20000"};
static const char *OPTS_CODEC[] = {"h264", "hevc"};
static const char *OPTS_PACKET[] = {"1024", "1400", "default"};
static const char *OPTS_LAYOUT[] = {"nintendo", "xbox", "custom"};
static const char *OPTS_FACE[] = {"A", "B", "X", "Y"};
static const char *OPTS_QUIT[] = {"back,start", "guide", "x,b", "leftshoulder,rightshoulder,start", "none"};

#define OPT_COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

typedef struct {
    const char *label;
    const char **opts;      /* NULL for text rows and action rows */
    int nopts;
    bool text;
    bool action;
    const char *charset;
} Row;

static const Row ROWS[R_COUNT] = {
    [R_ACTION]  = {"Action",       OPTS_ACTION,  OPT_COUNT(OPTS_ACTION),  false, false, NULL},
    [R_HOST]    = {"PC address",   NULL, 0,                               true,  false, "0123456789."},
    [R_APP]     = {"App on PC",    NULL, 0,                               true,  false, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-"},
    [R_RES]     = {"Resolution",   OPTS_RES,     OPT_COUNT(OPTS_RES),     false, false, NULL},
    [R_FPS]     = {"Frame rate",   OPTS_FPS,     OPT_COUNT(OPTS_FPS),     false, false, NULL},
    [R_BITRATE] = {"Bitrate",      OPTS_BITRATE, OPT_COUNT(OPTS_BITRATE), false, false, NULL},
    [R_CODEC]   = {"Codec",        OPTS_CODEC,   OPT_COUNT(OPTS_CODEC),   false, false, NULL},
    [R_PACKET]  = {"Packet size",  OPTS_PACKET,  OPT_COUNT(OPTS_PACKET),  false, false, NULL},
    [R_LAYOUT]  = {"Face buttons", OPTS_LAYOUT,  OPT_COUNT(OPTS_LAYOUT),  false, false, NULL},
    [R_MAPA]    = {"A (right) ->", OPTS_FACE,    OPT_COUNT(OPTS_FACE),    false, false, NULL},
    [R_MAPB]    = {"B (bottom) ->",OPTS_FACE,    OPT_COUNT(OPTS_FACE),    false, false, NULL},
    [R_MAPX]    = {"X (top) ->",   OPTS_FACE,    OPT_COUNT(OPTS_FACE),    false, false, NULL},
    [R_MAPY]    = {"Y (left) ->",  OPTS_FACE,    OPT_COUNT(OPTS_FACE),    false, false, NULL},
    [R_QUIT]    = {"Quit combo",   OPTS_QUIT,    OPT_COUNT(OPTS_QUIT),    false, false, NULL},
    [R_START]   = {"START",        NULL, 0,                               false, true,  NULL},
    [R_SAVE]    = {"Save and exit",NULL, 0,                               false, true,  NULL},
};

static int index_of(const char **opts, int n, const char *val) {
    for (int i = 0; i < n; ++i)
        if (strcasecmp(opts[i], val) == 0) return i;
    return 0;
}

/* The selection index for every row, derived from the settings once at start
   and kept in step from then on. */
static void sync_sel(const Settings *s, int sel[R_COUNT]) {
    char res[32];
    snprintf(res, sizeof res, "%sx%s", s->width, s->height);
    sel[R_ACTION] = index_of(OPTS_ACTION, OPT_COUNT(OPTS_ACTION), s->mode);
    sel[R_RES] = index_of(OPTS_RES, OPT_COUNT(OPTS_RES), res);
    sel[R_FPS] = index_of(OPTS_FPS, OPT_COUNT(OPTS_FPS), s->fps);
    sel[R_BITRATE] = index_of(OPTS_BITRATE, OPT_COUNT(OPTS_BITRATE), s->bitrate);
    sel[R_CODEC] = index_of(OPTS_CODEC, OPT_COUNT(OPTS_CODEC), s->codec);
    sel[R_PACKET] = index_of(OPTS_PACKET, OPT_COUNT(OPTS_PACKET),
                             s->packetsize[0] ? s->packetsize : "default");
    sel[R_LAYOUT] = index_of(OPTS_LAYOUT, OPT_COUNT(OPTS_LAYOUT), s->layout);
    for (int i = 0; i < 4; ++i) sel[R_MAPA + i] = s->map[i];
    sel[R_QUIT] = index_of(OPTS_QUIT, OPT_COUNT(OPTS_QUIT),
                           s->quitcombo[0] ? s->quitcombo : "none");
}

static void row_value(const Settings *s, const int sel[R_COUNT], int r, char *out, size_t n) {
    if (ROWS[r].text) {
        const char *v = r == R_HOST ? s->host : s->app;
        snprintf(out, n, "%s", v[0] ? v : "(empty)");
        return;
    }
    if (ROWS[r].action) { snprintf(out, n, "%s", r == R_START ? "run" : "write"); return; }
    snprintf(out, n, "%s", ROWS[r].opts[sel[r]]);
}

/* Left/right on a row. Returns true when something changed. */
static void row_turn(Settings *s, int sel[R_COUNT], int r, int dir) {
    if (ROWS[r].text || ROWS[r].action) return;
    int n = ROWS[r].nopts;
    sel[r] = (sel[r] + (dir > 0 ? 1 : n - 1)) % n;

    switch (r) {
    case R_ACTION:
        copy_str(s->mode, sizeof s->mode, ROWS[r].opts[sel[r]]);
        break;
    case R_RES: {
        int w = 0, h = 0;
        sscanf(ROWS[r].opts[sel[r]], "%dx%d", &w, &h);
        snprintf(s->width, sizeof s->width, "%d", w);
        snprintf(s->height, sizeof s->height, "%d", h);
        break;
    }
    case R_FPS:     copy_str(s->fps, sizeof s->fps, ROWS[r].opts[sel[r]]); break;
    case R_BITRATE: copy_str(s->bitrate, sizeof s->bitrate, ROWS[r].opts[sel[r]]); break;
    case R_CODEC:   copy_str(s->codec, sizeof s->codec, ROWS[r].opts[sel[r]]); break;
    case R_PACKET:
        copy_str(s->packetsize, sizeof s->packetsize,
                 strcmp(ROWS[r].opts[sel[r]], "default") == 0 ? "" : ROWS[r].opts[sel[r]]);
        break;
    case R_LAYOUT:
        copy_str(s->layout, sizeof s->layout, ROWS[r].opts[sel[r]]);
        apply_layout(s, s->layout);
        sync_sel(s, sel);
        break;
    case R_MAPA: case R_MAPB: case R_MAPX: case R_MAPY:
        /* Pointing one button by hand is a custom layout, whatever the preset
           said -- otherwise the table and the preset would disagree about what
           the screen is showing. */
        s->map[r - R_MAPA] = sel[r];
        copy_str(s->layout, sizeof s->layout, "custom");
        sel[R_LAYOUT] = index_of(OPTS_LAYOUT, OPT_COUNT(OPTS_LAYOUT), "custom");
        break;
    case R_QUIT:
        copy_str(s->quitcombo, sizeof s->quitcombo,
                 strcmp(ROWS[r].opts[sel[r]], "none") == 0 ? "" : ROWS[r].opts[sel[r]]);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------- text editor */

static int editing_row = -1;
static int edit_cursor = 0;
static char edit_backup[STR];

static char *field_ptr(Settings *s, int r) { return r == R_HOST ? s->host : s->app; }

static void edit_start(Settings *s, int r) {
    editing_row = r;
    edit_cursor = 0;
    snprintf(edit_backup, sizeof edit_backup, "%s", field_ptr(s, r));
}

static void edit_finish(void) { editing_row = -1; }

static void edit_cancel(Settings *s) {
    if (editing_row >= 0) snprintf(field_ptr(s, editing_row), STR, "%s", edit_backup);
    editing_row = -1;
}

static void edit_move(Settings *s, int dir) {
    if (editing_row < 0) return;
    size_t len = strlen(field_ptr(s, editing_row));
    if (dir < 0 && edit_cursor > 0) --edit_cursor;
    if (dir > 0 && edit_cursor < (int)len) ++edit_cursor;
}

static void edit_char(Settings *s, int dir) {
    if (editing_row < 0) return;
    char *field = field_ptr(s, editing_row);
    const char *set = ROWS[editing_row].charset;
    int n = (int)strlen(set);
    char c = (edit_cursor < (int)strlen(field)) ? field[edit_cursor] : 0;
    int at = c ? (int)(strchr(set, c) - set) : n;   /* n is the "nothing here" slot */
    if (at < 0 || at > n) at = n;
    at = (at + (dir > 0 ? 1 : n)) % (n + 1);
    char next = at == n ? 0 : set[at];
    if (next == 0) {
        if ((size_t)edit_cursor < strlen(field)) field[edit_cursor] = 0;   /* truncate here */
    } else if ((size_t)edit_cursor >= strlen(field)) {
        if (strlen(field) >= STR - 1) return;        /* no room to grow */
        field[edit_cursor] = next;
        field[edit_cursor + 1] = 0;
    } else {
        field[edit_cursor] = next;
    }
}

/* ------------------------------------------------------------------ drawing */

static bool readable(const char *path) { return path && path[0] && access(path, R_OK) == 0; }

static bool join_readable(char *out, size_t n, const char *root, const char *rel) {
    if (!root || !root[0] || !rel || !rel[0]) return false;
    int k = snprintf(out, n, "%s/%s", root, rel);
    return k > 0 && (size_t)k < n && readable(out);
}

static bool resolve_font(char *out, size_t n) {
    const char *abs = getenv("ML_NOTICE_FONT");
    if (readable(abs)) { snprintf(out, n, "%s", abs); return true; }
    const char *rel = getenv("CAT_FONT_PATH");
    if (rel && rel[0] == '/' && readable(rel)) { snprintf(out, n, "%s", rel); return true; }
    if (join_readable(out, n, getenv("CAT_FONTS_DIR"), rel)) return true;
    const char *launcher = getenv("UMRK_LAUNCHER_PATH");
    if (join_readable(out, n, launcher, "res/font.ttf")) return true;
    if (join_readable(out, n, launcher, "res/fonts/SpaceGrotesk/SpaceGrotesk-Regular.ttf")) return true;
    return false;
}

static void draw(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y, SDL_Color c, bool right) {
    if (!f || !s || !s[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = {right ? x - surf->w : x, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void draw_centre(SDL_Renderer *r, TTF_Font *f, const char *s, int W, int y, SDL_Color c) {
    if (!f || !s || !s[0]) return;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(f, s, &w, &h) != 0) return;
    draw(r, f, s, (W - w) / 2, y, c, false);
}

/* A point size picked by eye is what let the text run through the rule at the
   foot of its row on the device, and the way out is to stop picking it: this
   measures the real glyph box and steps the size down until it fits the box it
   has to sit in. Measuring rather than computing matters because a point size
   says nothing about how much room a given font wants -- one keeps its tall
   accents inside the size, another does not. */
static TTF_Font *fit_font(const char *path, int box_h, int min_px) {
    /* Caps, descenders and digits: whichever of these is tallest is what has
       to clear the row above and below it. */
    const char *probe = "\xc3\x81gjpqyQ0159";
    const SDL_Color white = {255, 255, 255, 255};
    if (box_h < min_px) box_h = min_px;
    for (int px = box_h; px >= min_px; --px) {
        TTF_Font *f = TTF_OpenFont(path, px);
        if (!f) continue;
        SDL_Surface *s = TTF_RenderUTF8_Blended(f, probe, white);
        int h = s ? s->h : 0;
        if (s) SDL_FreeSurface(s);
        if (h > 0 && h <= box_h) return f;
        TTF_CloseFont(f);
    }
    return TTF_OpenFont(path, min_px);
}

/* Where the top of the text goes so that it sits in the middle of its box.
   Measured from the font, not from the string, so a row whose text happens to
   have no descenders does not sit higher than the rest. */
static int box_text_y(TTF_Font *f, int top, int box_h) {
    int h = f ? TTF_FontHeight(f) : 0;
    if (h <= 0 || h >= box_h) return top;
    return top + (box_h - h) / 2;
}

/* --------------------------------------------------------------------- input */

/* The file settings are written back to. Global because the input handler
   saves without being told where to -- the alternative is threading a path
   through every button press. */
static const char *conf_path = NULL;

/* ------------------------------------------------------------------- wiring */

/* Whether the button SDL reports is the button printed on the device.

   Every prompt on this screen is written for what is printed on the keys,
   because that is what you read when you look down at the pad, and the two
   are not always the same thing. The MLP1's pad is not in SDL's database, so
   SDL numbers its buttons the way Linux does, which is by position: 0 is the
   bottom button, 1 the one on the right. The device is laid out the way a
   Nintendo pad is, with A printed on the right and B at the bottom, so what
   SDL calls A is the button printed B -- and a screen that took SDL at its
   word put confirm on the key marked cancel, and the other way round.

   X and Y need no such swap: Linux numbers north before west, and X is printed
   on top and Y on the left.

   ML_PAD_WIRING says which it is outright, which is the way back on the day a
   device turns out to be wired the other way. */
static int pad_wiring[4] = {0, 1, 2, 3};   /* what SDL says -> what is printed */

static bool name_has(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; ++p)
        if (strncasecmp(p, needle, n) == 0) return true;
    return false;
}

static void pad_wiring_set(bool by_position) {
    pad_wiring[0] = by_position ? 1 : 0;
    pad_wiring[1] = by_position ? 0 : 1;
}

static void pad_wiring_init(void) {
    const char *w = getenv("ML_PAD_WIRING");
    if (w && w[0]) {
        bool by_pos = strcasecmp(w, "labels") != 0 && strcasecmp(w, "printed") != 0;
        pad_wiring_set(by_pos);
        fprintf(stderr, "pad wiring: %s\n", by_pos ? "by position" : "as printed");
        return;
    }
    pad_wiring_set(false);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        const char *name = SDL_GameControllerNameForIndex(i);
        if (!name) name = SDL_JoystickNameForIndex(i);
        if (name_has(name, "Loong")) {
            pad_wiring_set(true);
            fprintf(stderr, "pad wiring: %s is not in SDL's database, numbered by position\n", name);
            return;
        }
    }
}

/* One press, as a d-pad or a face button would deliver it. Returns true when
   the screen is finished, in which case *action is what the launcher should do
   next and *exit_code is what the process should exit with.

   Kept apart from the drawing so the self-test can drive a whole session
   without a window: the part of this most likely to be wrong -- which button
   moves what -- is exactly the part a screenshot cannot check. */
static bool menu_event(Settings *s, int sel[R_COUNT], int *cur, const char **action,
                       int *exit_code, const SDL_Event *ev) {
    int dpad = 0, face_a = 0, face_b = 0, start = 0;

    if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
        /* Only the four face buttons carry a name that can disagree with the
           device; the rest of the pad is numbered the same either way. */
        int b = ev->cbutton.button;
        if (b >= 0 && b <= 3) b = pad_wiring[b];
        switch ((SDL_GameControllerButton) b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: dpad = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: dpad = 2; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: dpad = 3; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dpad = 4; break;
        case SDL_CONTROLLER_BUTTON_A: face_a = 1; break;
        case SDL_CONTROLLER_BUTTON_B: face_b = 1; break;
        case SDL_CONTROLLER_BUTTON_START: start = 1; break;
        default: return false;
        }
    } else if (ev->type == SDL_KEYDOWN) {
        switch (ev->key.keysym.sym) {
        case SDLK_UP: dpad = 1; break;
        case SDLK_DOWN: dpad = 2; break;
        case SDLK_LEFT: dpad = 3; break;
        case SDLK_RIGHT: dpad = 4; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: face_a = 1; break;
        case SDLK_ESCAPE: face_b = 1; break;
        case SDLK_F5: start = 1; break;
        default: return false;
        }
    } else if (ev->type == SDL_QUIT) {
        *action = "none";
        return true;
    } else {
        return false;
    }

    if (editing_row >= 0) {
        if (dpad == 1 || dpad == 2) edit_char(s, dpad == 1 ? 1 : -1);
        else if (dpad == 3) edit_move(s, -1);
        else if (dpad == 4) edit_move(s, 1);
        else if (face_a) edit_finish();
        else if (face_b) { edit_cancel(s); sync_sel(s, sel); }
        return false;
    }

    if (dpad == 1) { if (*cur > 0) --*cur; }
    else if (dpad == 2) { if (*cur < R_COUNT - 1) ++*cur; }
    else if (dpad == 3) row_turn(s, sel, *cur, -1);
    else if (dpad == 4) row_turn(s, sel, *cur, 1);
    else if (face_a) {
        if (ROWS[*cur].text) edit_start(s, *cur);
        else if (*cur == R_START) {
            *action = s->mode[0] ? s->mode : "stream";
            if (save_conf(s, conf_path) != 0) {
                fprintf(stderr, "moonlight-menu: could not write %s\n", conf_path);
                *exit_code = 3;
            }
            return true;
        } else if (*cur == R_SAVE) {
            if (save_conf(s, conf_path) != 0) {
                fprintf(stderr, "moonlight-menu: could not write %s\n", conf_path);
                *exit_code = 3;
            }
            return true;
        } else row_turn(s, sel, *cur, 1);
    } else if (face_b) {
        /* Out without saving: a settings screen that has to be confirmed to be
           left is a screen you cannot get out of on a handheld. */
        return true;
    } else if (start) {
        *action = s->mode[0] ? s->mode : "stream";
        if (save_conf(s, conf_path) != 0) {
            fprintf(stderr, "moonlight-menu: could not write %s\n", conf_path);
            *exit_code = 3;
        }
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------- main */

static SDL_Event key_event(SDL_Keycode k) {
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = k;
    return ev;
}

static SDL_Event pad_event(SDL_GameControllerButton b) {
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_CONTROLLERBUTTONDOWN;
    ev.cbutton.button = (Uint8) b;
    return ev;
}

/* Drives the screen the way a thumb would and checks what it wrote. It saves
   into the conf it was given, so the build hands it a copy. */
static int selftest(const char *conf, const char *out) {
    conf_path = conf;
    (void)out;

    Settings s;
    defaults(&s);
    load_conf(&s, conf);
    printf("read: host=%s app=%s mode=%s %sx%s@%s bitrate=%s codec=%s packet=%s layout=%s quit=%s\n",
           s.host, s.app, s.mode, s.width, s.height, s.fps, s.bitrate, s.codec,
           s.packetsize, s.layout, s.quitcombo);
    printf("map: A->%s B->%s X->%s Y->%s\n",
           FACE_NAMES[s.map[0]], FACE_NAMES[s.map[1]], FACE_NAMES[s.map[2]], FACE_NAMES[s.map[3]]);

    char host_before[STR];
    snprintf(host_before, sizeof host_before, "%s", s.host);

    int sel[R_COUNT] = {0};
    sync_sel(&s, sel);
    printf("rows: action=%s res=%s fps=%s bitrate=%s codec=%s packet=%s layout=%s quit=%s\n",
           OPTS_ACTION[sel[R_ACTION]], OPTS_RES[sel[R_RES]], OPTS_FPS[sel[R_FPS]],
           OPTS_BITRATE[sel[R_BITRATE]], OPTS_CODEC[sel[R_CODEC]], OPTS_PACKET[sel[R_PACKET]],
           OPTS_LAYOUT[sel[R_LAYOUT]], OPTS_QUIT[sel[R_QUIT]]);

    int cur = R_ACTION;
    const char *action = "none";
    int exit_code = 0;
    bool done = false;
    SDL_Event ev;

#define PRESS(k) do { ev = key_event(k); \
        done = menu_event(&s, sel, &cur, &action, &exit_code, &ev); } while (0)

    /* Down to Face buttons, right twice to reach custom, one more down to the
       printed A, right once so it gives B. If any press lands on the wrong row
       the map below is not the one that gets saved. */
    for (int i = 0; i < R_LAYOUT && !done; ++i) PRESS(SDLK_DOWN);
    PRESS(SDLK_RIGHT);
    PRESS(SDLK_RIGHT);
    printf("at '%s': layout=%s map: A->%s B->%s X->%s Y->%s\n", ROWS[cur].label, s.layout,
           FACE_NAMES[s.map[0]], FACE_NAMES[s.map[1]], FACE_NAMES[s.map[2]], FACE_NAMES[s.map[3]]);
    PRESS(SDLK_DOWN);
    PRESS(SDLK_RIGHT);
    printf("at '%s': layout=%s map: A->%s B->%s X->%s Y->%s\n", ROWS[cur].label, s.layout,
           FACE_NAMES[s.map[0]], FACE_NAMES[s.map[1]], FACE_NAMES[s.map[2]], FACE_NAMES[s.map[3]]);

    /* Editing a text row and abandoning it has to leave the value alone. */
    while (cur > R_HOST && !done) PRESS(SDLK_UP);
    PRESS(SDLK_RETURN);
    printf("editing: %s\n", editing_row >= 0 ? ROWS[editing_row].label : "(nothing)");
    PRESS(SDLK_DOWN);
    printf("typed:   host=%s\n", s.host);
    PRESS(SDLK_ESCAPE);
    printf("cancelled, host=%s\n", s.host);
    if (strcmp(s.host, host_before) != 0) {
        printf("FAIL: cancel did not restore the host\n");
        return 1;
    }

    /* The device sends controller events, not key events, so walk a little of
       the same path with those too. */
    ev = pad_event(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    done = menu_event(&s, sel, &cur, &action, &exit_code, &ev);
    ev = pad_event(SDL_CONTROLLER_BUTTON_A);
    done = menu_event(&s, sel, &cur, &action, &exit_code, &ev);
    printf("pad A at '%s': editing=%s\n", ROWS[cur].label,
           editing_row >= 0 ? ROWS[editing_row].label : "(nothing)");
    ev = pad_event(SDL_CONTROLLER_BUTTON_B);
    done = menu_event(&s, sel, &cur, &action, &exit_code, &ev);
    printf("pad B: editing=%s\n", editing_row >= 0 ? ROWS[editing_row].label : "(nothing)");
    if (editing_row >= 0) { printf("FAIL: pad B did not leave the editor\n"); return 1; }

    /* The device does not report the letters it prints, so drive the screen
       the way it is actually wired -- SDL's B is the key printed A -- and
       check that the printed A is the one that confirms. */
    pad_wiring_set(true);
    while (cur > R_HOST && !done) PRESS(SDLK_UP);
    ev = pad_event(SDL_CONTROLLER_BUTTON_B);   /* printed A, the right-hand key */
    done = menu_event(&s, sel, &cur, &action, &exit_code, &ev);
    printf("printed A at '%s': editing=%s\n", ROWS[cur].label,
           editing_row >= 0 ? ROWS[editing_row].label : "(nothing)");
    if (editing_row != R_HOST) { printf("FAIL: the printed A did not start editing\n"); return 1; }
    ev = pad_event(SDL_CONTROLLER_BUTTON_A);   /* printed B, the bottom key */
    done = menu_event(&s, sel, &cur, &action, &exit_code, &ev);
    printf("printed B: editing=%s host=%s\n",
           editing_row >= 0 ? ROWS[editing_row].label : "(nothing)", s.host);
    if (editing_row >= 0 || strcmp(s.host, host_before) != 0) {
        printf("FAIL: the printed B did not cancel the edit\n");
        return 1;
    }
    pad_wiring_set(false);

    /* START anywhere saves and names what to do next. */
    PRESS(SDLK_F5);
    printf("start: done=%d action=%s exit=%d\n", done, action, exit_code);
    if (!done || strcmp(action, "stream") != 0 || exit_code != 0) {
        printf("FAIL: start did not save and run\n");
        return 1;
    }
#undef PRESS

    FILE *f = fopen(conf, "r");
    if (!f) { printf("FAIL: nothing written\n"); return 1; }
    char line[512];
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "MODE=", 5) == 0 || strncmp(line, "PAD_", 4) == 0 ||
            strncmp(line, "WIDTH=", 6) == 0)
            fputs(line, stdout);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *conf = NULL, *out = NULL, *font_arg = NULL;
    int timeout_ms = 0;   /* 0 = wait for ever */
    bool test = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--conf") == 0 && i + 1 < argc) conf = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) font_arg = argv[++i];
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) timeout_ms = atoi(argv[++i]) * 1000;
        else if (strcmp(argv[i], "--selftest") == 0) test = true;
    }
    if (!conf || !out) {
        fprintf(stderr, "usage: moonlight-menu --conf FILE --out FILE [--font PATH] [--timeout SECONDS] [--selftest]\n");
        return 2;
    }

    if (test) return selftest(conf, out);

    conf_path = conf;

    Settings s;
    defaults(&s);
    load_conf(&s, conf);
    int sel[R_COUNT] = {0};
    sync_sel(&s, sel);

    const char *action = "none";
    int exit_code = 0;

    /* The screen is where PAD_WIRING is chosen, so it is also where it has to
       be put for the streamer to read -- what the device reports is decided
       once, and both halves have to agree on it. */
    if (s.wiring[0] && !getenv("ML_PAD_WIRING")) setenv("ML_PAD_WIRING", s.wiring, 1);

    char font_path[1024];
    if (font_arg && readable(font_arg)) snprintf(font_path, sizeof font_path, "%s", font_arg);
    else if (!resolve_font(font_path, sizeof font_path)) font_path[0] = 0;
    if (font_path[0]) setenv("ML_NOTICE_FONT", font_path, 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "moonlight-menu: SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    pad_wiring_init();

    SDL_Window *win = SDL_CreateWindow("Moonlight settings", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 960, 720,
                                       SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) { fprintf(stderr, "moonlight-menu: window: %s\n", SDL_GetError()); SDL_Quit(); return 2; }
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!r) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!r) { fprintf(stderr, "moonlight-menu: renderer: %s\n", SDL_GetError()); SDL_DestroyWindow(win); SDL_Quit(); return 2; }

    int W = 960, H = 720;
    SDL_GetRendererOutputSize(r, &W, &H);

    /* Rows are laid out from what is left of the screen once the header and
       the footer have taken their share. Every row is visible at once when the
       screen is tall enough for it -- a settings screen you have to scroll to
       read is a settings screen where you forget what is at the bottom -- and
       when it is not, they scroll. The row height decides everything else:
       sizes below come from it, so the fits below hold on any screen. */
    const int pad = 24;
    const int head_h = H * 9 / 100;
    const int foot_h = H * 10 / 100;
    const int avail = H - head_h - foot_h;
    int row_h = avail / R_COUNT;
    if (row_h > 44) row_h = 44;          /* taller looks empty, not generous */
    if (row_h < 22) row_h = 22;          /* below this it stops being readable */

    int visible = avail / row_h;
    if (visible > R_COUNT) visible = R_COUNT;
    const int list_top = head_h + (avail - visible * row_h) / 2;

    /* Three sizes, each fitted to the box it lives in: the row keeps air above
       and below so the rule at its foot has somewhere to be drawn, and the
       header and footer shrink with the row so they never crowd it. */
    TTF_Font *f_title = NULL, *f_row = NULL, *f_hint = NULL;
    if (font_path[0] && TTF_Init() == 0) {
        f_row = fit_font(font_path, row_h * 68 / 100, 11);
        f_title = fit_font(font_path, (head_h - 20) * 80 / 100, 12);
        f_hint = fit_font(font_path, (foot_h - 18) / 2 * 72 / 100, 9);
    }
    if (!f_row) {
        fprintf(stderr, "moonlight-menu: no usable font (%s)\n", font_path[0] ? font_path : "none");
        if (f_title) TTF_CloseFont(f_title);
        SDL_DestroyRenderer(r); SDL_DestroyWindow(win); SDL_Quit();
        return 2;
    }
    const int row_text_h = TTF_FontHeight(f_row);

    const SDL_Color bg = {12, 12, 16, 255};
    const SDL_Color panel = {26, 26, 32, 255};
    const SDL_Color line = {52, 52, 64, 255};
    const SDL_Color hi = {58, 96, 148, 255};
    const SDL_Color text = {238, 238, 244, 255};
    const SDL_Color dim = {150, 150, 164, 255};
    const SDL_Color key = {255, 214, 120, 255};


    int cur = R_ACTION;
    int scroll = 0;
    bool done = false;
    Uint32 deadline = timeout_ms ? SDL_GetTicks() + (Uint32)timeout_ms : 0;

    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (menu_event(&s, sel, &cur, &action, &exit_code, &ev)) done = true;
        }

        if (deadline && SDL_GetTicks() >= deadline) { action = "none"; done = true; }

        if (cur < scroll) scroll = cur;
        if (cur >= scroll + visible) scroll = cur - visible + 1;

        SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, 255);
        SDL_RenderClear(r);

        SDL_Rect box = {pad / 2, 6, W - pad, head_h - 18};
        SDL_SetRenderDrawColor(r, panel.r, panel.g, panel.b, 255);
        SDL_RenderFillRect(r, &box);
        draw(r, f_title, "MOONLIGHT", pad + 8, box_text_y(f_title, box.y, box.h), text, false);
        draw(r, f_hint, "MLP1 settings", W - pad - 8, box_text_y(f_hint, box.y, box.h), dim, true);

        for (int i = scroll; i < R_COUNT && i < scroll + visible; ++i) {
            int y = list_top + (i - scroll) * row_h;
            if (i == cur) {
                SDL_Rect hl = {pad / 2, y - 2, W - pad, row_h - 2};
                SDL_SetRenderDrawColor(r, hi.r, hi.g, hi.b, 255);
                SDL_RenderFillRect(r, &hl);
            }
            /* The four mapping rows only speak when the layout is custom;
               otherwise the preset above them is what decides. */
            bool greyed = (i >= R_MAPA && i <= R_MAPY) && strcasecmp(s.layout, "custom") != 0;
            SDL_Color c = greyed ? dim : text;
            /* Both columns share one baseline, taken from the row rather than
               from either string, so nothing here can grow into the rule. */
            const int ty = box_text_y(f_row, y, row_h);
            draw(r, f_row, ROWS[i].label, pad + 8, ty, c, false);

            char val[STR];
            row_value(&s, sel, i, val, sizeof val);
            int vx = W - pad - 8;

            if (editing_row == i) {
                char prefix[STR];
                snprintf(prefix, sizeof prefix, "%s", field_ptr(&s, i));
                if (edit_cursor < (int)strlen(prefix)) prefix[edit_cursor] = 0;
                int pw = 0, ph = 0;
                TTF_SizeUTF8(f_row, prefix, &pw, &ph);
                (void)ph;
                SDL_Rect cur_box = {vx - pw - 12, ty + row_text_h + 1, 12, 3};
                SDL_SetRenderDrawColor(r, key.r, key.g, key.b, 255);
                SDL_RenderFillRect(r, &cur_box);
                draw(r, f_row, val, vx, ty, key, true);
            } else {
                draw(r, f_row, val, vx, ty, c, true);
            }

            SDL_SetRenderDrawColor(r, line.r, line.g, line.b, 255);
            SDL_RenderDrawLine(r, pad / 2, y + row_h - 1, W - pad / 2, y + row_h - 1);
        }

        /* Two lines of help, each centred in the half of the footer that is
           theirs. They follow the row down so they never outgrow it either. */
        const int hline_h = (foot_h - 16) / 2;
        SDL_Rect fbox = {pad / 2, H - foot_h + 6, W - pad, foot_h - 16};
        SDL_SetRenderDrawColor(r, panel.r, panel.g, panel.b, 255);
        SDL_RenderFillRect(r, &fbox);
        const int hint1 = box_text_y(f_hint, fbox.y, hline_h);
        const int hint2 = box_text_y(f_hint, fbox.y + hline_h, hline_h);
        if (editing_row >= 0) {
            draw_centre(r, f_hint, "Up/Down: character    Left/Right: move    A: done    B: cancel",
                        W, hint1, dim);
            draw_centre(r, f_hint, "The empty character ends the text", W, hint2, dim);
        } else {
            draw_centre(r, f_hint, "Up/Down: row    Left/Right: change    A: edit or run    B: exit",
                        W, hint1, dim);
            draw_centre(r, f_hint, "START anywhere: save and run", W, hint2, dim);
        }

        SDL_RenderPresent(r);
        SDL_Delay(16);
    }

    FILE *f = fopen(out, "w");
    if (f) { fprintf(f, "%s\n", action); fclose(f); }
    fprintf(stderr, "moonlight-menu: action=%s\n", action);

    if (f_title) TTF_CloseFont(f_title);
    if (f_row) TTF_CloseFont(f_row);
    if (f_hint) TTF_CloseFont(f_hint);
    if (f_title || f_row || f_hint) TTF_Quit();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return exit_code;
}
