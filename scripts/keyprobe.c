// SPDX-License-Identifier: MIT
//
// moonlight-keyprobe: report what a handheld's buttons actually are.
//
// The MLP1 has no keyboard, so upstream's quit combination -- Ctrl+Alt+Shift+Q
// -- cannot be pressed on it, and the gamepad combination wants a Start and a
// Select the device does not have. Choosing a replacement needs to know what
// the buttons really are, and "the d-pad is x, y, b and a" is not something
// to guess at when being wrong costs another cross-build.
//
// So: open every joystick, report the name SDL gives it and whether SDL counts
// it as a game controller, then echo every key and button as it is pressed,
// on screen and to stdout. stdout is the part that matters -- it lands in the
// launch log, which can be read off the SD card on a real computer.
//
//   moonlight-keyprobe [--seconds N]
//
// It stops by itself after N seconds. A program whose only job is to find out
// which buttons exist must not require a button that does not exist to be
// told to stop.
#define _POSIX_C_SOURCE 200809L   // setenv, strtok_r, access with -std=c11

#include <SDL.h>
#include <SDL_ttf.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINES 12
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

/* Same resolution order as the notice program: this pak sits beside the
   launcher and borrows its font rather than shipping one of its own. */
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

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *s, int x, int y, SDL_Color c) {
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = {x, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

/* Most recent first, which is what makes it usable: the button just pressed is
   the one whose name is being looked for. */
static void push_line(char lines[][MAX_TEXT], int *count, const char *text) {
    for (int i = MAX_LINES - 1; i > 0; --i)
        snprintf(lines[i], MAX_TEXT, "%s", lines[i - 1]);
    snprintf(lines[0], MAX_TEXT, "%s", text);
    if (*count < MAX_LINES) (*count)++;
}

int main(int argc, char **argv) {
    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            int n = atoi(argv[++i]);
            if (n > 0 && n <= 600) seconds = n;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    char font_path[1024];
    if (!resolve_font(font_path, sizeof font_path)) font_path[0] = 0;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "keyprobe: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("Keyprobe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       960, 720, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) {
        fprintf(stderr, "keyprobe: window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!r) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!r) {
        fprintf(stderr, "keyprobe: renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    int W = 960, H = 720;
    SDL_GetRendererOutputSize(r, &W, &H);
    printf("screen: %dx%d\n", W, H);

    TTF_Font *font = NULL;
    if (font_path[0] && TTF_Init() == 0)
        font = TTF_OpenFont(font_path, H / 28 < 16 ? 16 : H / 28);
    if (!font) fprintf(stderr, "keyprobe: no usable font (%s)\n", font_path[0] ? font_path : "none");

    /* What SDL thinks is plugged in. A device that is not a game controller
       produces no controller events at all, which is the single fact that
       decides whether upstream's Start+Select+LB+RB quit can ever fire. */
    int joysticks = SDL_NumJoysticks();
    printf("joysticks: %d\n", joysticks);
    for (int i = 0; i < joysticks; ++i) {
        const char *name = SDL_JoystickNameForIndex(i);
        printf("joystick %d: name=%s controller=%s\n", i, name ? name : "(unnamed)",
               SDL_IsGameController(i) ? "yes" : "no");
        if (SDL_IsGameController(i)) {
            SDL_GameController *c = SDL_GameControllerOpen(i);
            if (c) {
                char *mapping = SDL_GameControllerMapping(c);
                printf("joystick %d: mapping=%s\n", i, mapping ? mapping : "(none)");
                SDL_free(mapping);
            } else {
                printf("joystick %d: opened=no (%s)\n", i, SDL_GetError());
            }
        } else {
            SDL_JoystickOpen(i);   // still listen, to report raw buttons
        }
    }
    fflush(stdout);

    char lines[MAX_LINES][MAX_TEXT];
    int count = 0;
    for (int i = 0; i < MAX_LINES; ++i) lines[i][0] = 0;

    Uint32 deadline = SDL_GetTicks() + (Uint32)seconds * 1000;
    while (!g_stop) {
        SDL_Event ev;
        bool got = false;
        while (SDL_PollEvent(&ev)) {
            char buf[MAX_TEXT];
            switch (ev.type) {
            case SDL_KEYDOWN:
                if (ev.key.repeat) break;
                snprintf(buf, sizeof buf, "key %s (code %d)",
                         SDL_GetKeyName(ev.key.keysym.sym), (int)ev.key.keysym.sym);
                printf("%s\n", buf);
                push_line(lines, &count, buf);
                got = true;
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                snprintf(buf, sizeof buf, "pad button %s (%d)",
                         SDL_GameControllerGetStringForButton(
                             (SDL_GameControllerButton)ev.cbutton.button),
                         ev.cbutton.button);
                printf("%s\n", buf);
                push_line(lines, &count, buf);
                got = true;
                break;
            case SDL_JOYBUTTONDOWN:
                snprintf(buf, sizeof buf, "joy %d button %d", ev.jbutton.which, ev.jbutton.button);
                printf("%s\n", buf);
                push_line(lines, &count, buf);
                got = true;
                break;
            case SDL_JOYHATMOTION:
                snprintf(buf, sizeof buf, "joy %d hat %d = %d", ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                printf("%s\n", buf);
                push_line(lines, &count, buf);
                got = true;
                break;
            default:
                break;
            }
        }
        if (got) fflush(stdout);
        if (SDL_GetTicks() >= deadline) break;

        int remaining = (int)(deadline - SDL_GetTicks()) / 1000;
        SDL_SetRenderDrawColor(r, 12, 12, 16, 255);
        SDL_RenderClear(r);
        if (font) {
            const SDL_Color title_c = {245, 245, 245, 255};
            const SDL_Color body_c = {200, 200, 210, 255};
            const SDL_Color hint_c = {138, 138, 150, 255};
            int line_h = TTF_FontLineSkip(font);
            int y = H / 16;
            draw_text(r, font, "KEY PROBE - press every button", H / 24, y, title_c);
            y += line_h * 2;
            for (int i = 0; i < count; ++i) {
                draw_text(r, font, lines[i], H / 24, y, body_c);
                y += line_h;
            }
            char tail[MAX_TEXT];
            snprintf(tail, sizeof tail, "ends in %ds - names are in moonlight-probe.txt", remaining);
            draw_text(r, font, tail, H / 24, H - line_h * 2, hint_c);
        }
        SDL_RenderPresent(r);
        SDL_Delay(50);
    }

    printf("keyprobe: done\n");
    fflush(stdout);

    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
