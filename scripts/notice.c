// SPDX-License-Identifier: MIT
//
// moonlight-notice: a small fullscreen message for the Moonlight pak.
//
// Moonlight Embedded is a command-line program and Leaf gives a tool pak no
// error surface, so anything the binary printed was going nowhere the user
// could see. The wrapper uses this to show what happened -- most importantly
// the four-digit PIN a pairing asks for, which is otherwise invisible on a
// handheld with no terminal.
//
// Deliberately small: SDL2 and SDL_ttf only, both provided by the MLP1 (see
// pak/device-libs.txt). The font is the launcher's own, resolved the way
// DSperate-pak's notice program resolves it; the pak bundles no font.
//
//   moonlight-notice [--timeout SECONDS] [--font PATH] TITLE [BODY...]
//
// Exits 0 after an acknowledgement, the timeout, or SIGTERM/SIGINT, and 1 only
// when it could not open a window at all.
#define _POSIX_C_SOURCE 200809L   // setenv, strtok_r, access with -std=c11

#include <SDL.h>
#include <SDL_ttf.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINES 24
#define MAX_TEXT 512

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static bool readable(const char *path) {
    return path && path[0] && access(path, R_OK) == 0;
}

static bool join_readable(char *out, size_t n, const char *root, const char *rel) {
    if (!root || !root[0] || !rel || !rel[0]) return false;
    int k = snprintf(out, n, "%s/%s", root, rel);
    return k > 0 && (size_t)k < n && readable(out);
}

/* The launcher's font: an explicit override, an absolute CAT_FONT_PATH,
   CAT_FONTS_DIR plus the relative CAT_FONT_PATH, then the launcher res tree
   this pak always sits beside. Same order DSperate-pak's notice uses. */
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

static int text_width(TTF_Font *font, const char *s) {
    int w = 0, h = 0;
    return TTF_SizeUTF8(font, s, &w, &h) == 0 ? w : 0;
}

static int wrap_text(TTF_Font *font, const char *text, int max_w,
                     char lines[][MAX_TEXT], int max) {
    char buf[MAX_TEXT * 4];
    snprintf(buf, sizeof buf, "%s", text);
    char cur[MAX_TEXT];
    cur[0] = 0;
    int count = 0;
    char *save = NULL;
    for (char *word = strtok_r(buf, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
        char trial[MAX_TEXT];
        if (cur[0]) snprintf(trial, sizeof trial, "%s %s", cur, word);
        else snprintf(trial, sizeof trial, "%s", word);
        if (cur[0] && text_width(font, trial) > max_w) {
            if (count < max) snprintf(lines[count++], MAX_TEXT, "%s", cur);
            snprintf(cur, sizeof cur, "%s", word);
        } else {
            snprintf(cur, sizeof cur, "%s", trial);
        }
    }
    if (cur[0] && count < max) snprintf(lines[count++], MAX_TEXT, "%s", cur);
    return count;
}

static void fill(SDL_Renderer *r, SDL_Rect box, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &box);
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *s, int cx, int y, SDL_Color c) {
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = {cx - surf->w / 2, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

int main(int argc, char **argv) {
    int timeout_ms = 10000;
    const char *font_arg = NULL;
    const char *title = NULL;
    const char *body[MAX_LINES];
    int body_count = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            int secs = atoi(argv[++i]);
            if (secs > 0 && secs <= 600) timeout_ms = secs * 1000;
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            font_arg = argv[++i];
        } else if (!title) {
            title = argv[i];
        } else if (body_count < MAX_LINES) {
            body[body_count++] = argv[i];
        }
    }
    if (!title) {
        fprintf(stderr, "usage: moonlight-notice [--timeout SECONDS] [--font PATH] TITLE [BODY...]\n");
        return 2;
    }
    for (int i = 0; i < body_count; ++i) fprintf(stderr, "moonlight-notice: %s\n", body[i]);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    char font_path[1024];
    if (font_arg && readable(font_arg)) snprintf(font_path, sizeof font_path, "%s", font_arg);
    else if (!resolve_font(font_path, sizeof font_path)) font_path[0] = 0;
    if (font_path[0]) setenv("ML_NOTICE_FONT", font_path, 1);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "moonlight-notice: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_JoystickEventState(SDL_ENABLE);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) SDL_JoystickOpen(i);

    SDL_Window *win = SDL_CreateWindow("Moonlight", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       960, 720, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) {
        fprintf(stderr, "moonlight-notice: window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!r) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!r) {
        fprintf(stderr, "moonlight-notice: renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    int W = 960, H = 720;
    SDL_GetRendererOutputSize(r, &W, &H);

    TTF_Font *font_title = NULL, *font_body = NULL;
    if (font_path[0] && TTF_Init() == 0) {
        int ts = H / 14; if (ts < 20) ts = 20;
        int bs = H / 26; if (bs < 14) bs = 14;
        font_title = TTF_OpenFont(font_path, ts);
        font_body = TTF_OpenFont(font_path, bs);
    }
    if (!font_title)
        fprintf(stderr, "moonlight-notice: no usable font (%s)\n", font_path[0] ? font_path : "none");

    const int panel_w = W * 4 / 5;
    const int panel_x = (W - panel_w) / 2;
    const int max_text_w = panel_w - W / 12;
    const int line_h = font_body ? TTF_FontLineSkip(font_body) : H / 26;

    char body_lines[MAX_LINES][MAX_TEXT];
    int body_lines_n = 0;
    for (int i = 0; i < body_count && body_lines_n < MAX_LINES; ++i) {
        if (!font_body) {
            snprintf(body_lines[body_lines_n++], MAX_TEXT, "%s", body[i]);
        } else {
            body_lines_n += wrap_text(font_body, body[i], max_text_w,
                                      &body_lines[body_lines_n], MAX_LINES - body_lines_n);
        }
    }

    const int title_h = font_title ? TTF_FontHeight(font_title) : H / 14;
    const int hint_h = font_body ? TTF_FontHeight(font_body) : line_h;
    const int pad = H / 24;
    const int content_h = title_h + (body_lines_n > 0 ? pad / 2 + body_lines_n * line_h : 0) + pad + hint_h;
    const int panel_h = content_h + 2 * pad;
    const int panel_y = (H - panel_h) / 2;

    const SDL_Color bg = {12, 12, 16, 255};
    const SDL_Color panel = {28, 28, 34, 255};
    const SDL_Color border = {74, 74, 88, 255};
    const SDL_Color title_c = {245, 245, 245, 255};
    const SDL_Color body_c = {200, 200, 210, 255};
    const SDL_Color hint_c = {138, 138, 150, 255};

    Uint32 deadline = SDL_GetTicks() + (Uint32)timeout_ms;
    while (!g_stop) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || ev.type == SDL_KEYDOWN || ev.type == SDL_JOYBUTTONDOWN ||
                ev.type == SDL_MOUSEBUTTONDOWN) {
                g_stop = 1;
            }
        }
        if (SDL_GetTicks() >= deadline) break;

        SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, 255);
        SDL_RenderClear(r);
        SDL_Rect box = {panel_x, panel_y, panel_w, panel_h};
        fill(r, box, panel);
        SDL_SetRenderDrawColor(r, border.r, border.g, border.b, 255);
        SDL_RenderDrawRect(r, &box);

        int y = panel_y + pad;
        if (font_title) draw_text(r, font_title, title, W / 2, y, title_c);
        y += title_h + pad / 2;
        for (int i = 0; i < body_lines_n; ++i) {
            if (font_body) draw_text(r, font_body, body_lines[i], W / 2, y, body_c);
            y += line_h;
        }
        if (font_body)
            draw_text(r, font_body, "Press A or Menu to continue", W / 2,
                      panel_y + panel_h - pad - hint_h, hint_c);

        SDL_RenderPresent(r);
        SDL_Delay(100);
    }

    if (font_title) TTF_CloseFont(font_title);
    if (font_body) TTF_CloseFont(font_body);
    if (font_title || font_body) TTF_Quit();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
