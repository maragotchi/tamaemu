#include "emu.h"

#define VIEW_W   (PANEL_W * PANEL_SCALE)
#define VIEW_H   (PANEL_H * PANEL_SCALE)
#define BTN_R    22
#define BTN_CY   (VIEW_H + STRIP_H / 2)

static const int     BTN_CX[3]  = { VIEW_W / 6, VIEW_W / 2, VIEW_W * 5 / 6 };
static const uint8_t BTN_BIT[3] = { 1, 2, 4 };

int panel_hit(int x, int y)
{
    for (int i = 0; i < 3; i++) {
        int dx = x - BTN_CX[i], dy = y - BTN_CY;
        if (dx * dx + dy * dy <= BTN_R * BTN_R) return BTN_BIT[i];
    }
    return 0;
}

/* Lowercase ASCII values double as SDL keycodes; s and n are reserved. */
int panel_keys_parse(const char *s, char out[3])
{
    if (!s || !s[0] || !s[1] || !s[2] || s[3]) return 0;
    char k[3];
    for (int i = 0; i < 3; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (!((c >= 'a' && c <= 'z') || (c >= '1' && c <= '9'))) return 0;
        if (c == 's' || c == 'n') return 0;
        k[i] = c;
    }
    if (k[0] == k[1] || k[0] == k[2] || k[1] == k[2]) return 0;
    out[0] = k[0]; out[1] = k[1]; out[2] = k[2];
    return 1;
}

#ifdef USE_SDL
#include <SDL2/SDL.h>
#include <math.h>

/* SDL2 has no circle primitive. */
static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy));   /* match panel_hit's boundary */
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

/* Leaves the renderer color set; callers must reset it before clearing. */
void panel_draw(SDL_Renderer *ren, uint8_t pressed)
{
    SDL_Rect strip = { 0, VIEW_H, VIEW_W, STRIP_H };
    SDL_SetRenderDrawColor(ren, 44, 44, 42, 255);
    SDL_RenderFillRect(ren, &strip);

    for (int i = 0; i < 3; i++) {
        int lit = (pressed & BTN_BIT[i]) != 0;
        if (lit) SDL_SetRenderDrawColor(ren, 245, 244, 238, 255);
        else     SDL_SetRenderDrawColor(ren, 211, 209, 199, 255);
        fill_circle(ren, BTN_CX[i], BTN_CY, BTN_R);
    }
}
#endif
