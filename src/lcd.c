#include "emu.h"
#include <stdarg.h>
#include <string.h>

static const uint8_t ID_E0[16] = {0x03,0x04,0x08,0x25,0x22,0x0A,0x1E,0x02,0x02,0x04,0x0F,0x0F,0x00,0x04,0x03,0x06};
static const uint8_t ID_E1[16] = {0x0A,0x22,0x25,0x15,0x0F,0x0F,0x01,0x1E,0x06,0x04,0x05,0x01,0x0F,0x0F,0x04,0x02};
static const uint8_t PC_E0[16] = {0x00,0x03,0x0F,0x24,0x20,0x00,0x1D,0x03,0x00,0x05,0x0E,0x0E,0x01,0x04,0x04,0x06};

static void lcdlog(Emu *e, const char *fmt, ...)
{
    if (!e->lcd.log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(e->lcd.logf ? e->lcd.logf : stderr, fmt, ap);
    va_end(ap);
}

static void finish_cmd(Emu *e)
{
    Lcd *l = &e->lcd;
    if (!l->cmd) return;
    switch (l->cmd) {
    case 0x2A: if (l->argc >= 4) { l->xs = (l->args[0] << 8) | l->args[1];
                                   l->xe = (l->args[2] << 8) | l->args[3];
                                   l->cx = l->xs; } break;
    case 0x2B: if (l->argc >= 4) { l->ys = (l->args[0] << 8) | l->args[1];
                                   l->ye = (l->args[2] << 8) | l->args[3];
                                   l->cy = l->ys; } break;
    case 0xE0: if (l->argc >= 16) { memcpy(l->gamma_pos, l->args, 16); l->gp_n = 16; } break;
    case 0xE1: if (l->argc >= 16) { memcpy(l->gamma_neg, l->args, 16); l->gn_n = 16; } break;
    case 0xC0: if (l->argc >= 1) { l->pwr_c0[0] = l->args[0];
                                   l->pwr_c0[1] = l->argc >= 2 ? l->args[1] : 0;
                                   l->c0_n = l->argc > 2 ? 2 : l->argc; } break;
    case 0xC2: if (l->argc >= 1) { l->pwr_c2[0] = l->args[0]; l->c2_n = 1; } break;
    case 0xC5: if (l->argc >= 1) { l->vcom_c5[0] = l->args[0]; l->c5_n = 1; } break;
    case 0xC6: if (l->argc >= 1) { l->vcom_c6[0] = l->args[0]; l->c6_n = 1; } break;
    default: break;
    }
    if (l->cmd == 0x2C && l->argc) {
        lcdlog(e, "[lcd] RAMWR burst %d bytes (window %dx%d = %d px)\n",
               l->argc, l->xe - l->xs + 1, l->ye - l->ys + 1,
               (l->xe - l->xs + 1) * (l->ye - l->ys + 1));
    }
    if (l->argc && l->cmd != 0x2C) {
        lcdlog(e, "[lcd] cmd %02x args:", l->cmd);
        for (int i = 0; i < l->argc && i < 16; i++) lcdlog(e, " %02x", l->args[i]);
        lcdlog(e, "\n");
    }
}

void lcd_cmd(Emu *e, uint8_t c)
{
    Lcd *l = &e->lcd;
    finish_cmd(e);
    l->cmd = c; l->argc = 0; l->pix_n = 0;
    l->cmd_count++;
    switch (c) {
    case 0x01: /* SWRESET */
        l->madctl = 0; l->sleep_out = false; l->disp_on = false; l->inverted = false;
        l->xs = 0; l->xe = GRAM_W - 1; l->ys = 0; l->ye = GRAM_H - 1;
        lcdlog(e, "[lcd] cmd 01 SWRESET\n"); break;
    case 0x10: lcdlog(e, "[lcd] cmd 10 SLPIN cyc=%llu pc=%08x\n",
                      (unsigned long long)e->cycles, e->pc); l->sleep_out = false; break;
    case 0x11: lcdlog(e, "[lcd] cmd 11 SLPOUT cyc=%llu\n",
                      (unsigned long long)e->cycles); l->sleep_out = true; break;
    case 0x20: l->inverted = false; lcdlog(e, "[lcd] cmd 20 INVOFF\n"); break;
    case 0x21: l->inverted = true;  lcdlog(e, "[lcd] cmd 21 INVON\n"); break;
    case 0x28: l->disp_on = false; lcdlog(e, "[lcd] cmd 28 DISPOFF\n"); break;
    case 0x29: l->disp_on = true; l->frame_marker++; lcdlog(e, "[lcd] cmd 29 DISPON\n"); break;
    case 0x2C: l->cx = l->xs; l->cy = l->ys;
        lcdlog(e, "[lcd] cmd 2C RAMWR window x=%d..%d y=%d..%d madctl=%02x\n",
               l->xs, l->xe, l->ys, l->ye, l->madctl);
        break;
    default: break;
    }
}

static void put_pixel(Emu *e, uint16_t rgb565)
{
    Lcd *l = &e->lcd;
    /* Firmware MADCTL settings make addressed and panel coordinates identical. */
    int x = l->cx, y = l->cy;
    if (x >= 0 && x < GRAM_W && y >= 0 && y < GRAM_H)
        l->gram[y][x] = rgb565;
    if (++l->cx > l->xe) {
        l->cx = l->xs;
        if (++l->cy > l->ye) l->cy = l->ys;
    }
}

void lcd_data(Emu *e, uint8_t d)
{
    Lcd *l = &e->lcd;
    if (l->cmd == 0x2C) {
        l->argc++;   /* diagnostic byte count; pixels are not stored in args */
        l->ramwr_bytes++;
        /* COLMOD's low nibble selects RGB565 (5) or RGB666 (6). */
        int bpp = ((l->colmod & 0x0F) == 0x05) ? 2 : 3;
        l->pix_buf[l->pix_n++] = d;
        if (l->pix_n >= bpp) {
            uint16_t p;
            if (bpp == 2)
                p = (uint16_t)((l->pix_buf[0] << 8) | l->pix_buf[1]);
            else {
                /* Pack RGB666's six high bits per channel as RGB565. */
                uint32_t r6 = l->pix_buf[0] >> 2, g6 = l->pix_buf[1] >> 2, b6 = l->pix_buf[2] >> 2;
                p = (uint16_t)(((r6 >> 1) << 11) | (g6 << 5) | (b6 >> 1));
            }
            l->pix_n = 0;
            put_pixel(e, p);
        }
        return;
    }
    if (l->argc < 16) l->args[l->argc] = d;
    l->argc++;
    if (l->cmd == 0x36 && l->argc == 1) {
        l->madctl = d;
        lcdlog(e, "[lcd] cmd 36 MADCTL = %02x (MY=%d MX=%d MV=%d BGR=%d)\n",
               d, !!(d & 0x80), !!(d & 0x40), !!(d & 0x20), !!(d & 0x08));
    }
    if (l->cmd == 0x3A && l->argc == 1) {
        l->colmod = d;
        lcdlog(e, "[lcd] cmd 3A COLMOD = %02x\n", d);
    }
}

uint8_t lcd_read(Emu *e, uint32_t a)
{
    lcdlog(e, "[lcd] READ %08x (cmd context %02x) -> 00\n", a, e->lcd.cmd);
    return 0;
}

void lcd_render(Emu *e, uint32_t *out, int *w, int *h)
{
    Lcd *l = &e->lcd;
    int bgr = l->madctl & 0x08;
    *w = PANEL_W; *h = PANEL_H;
    /* Sleeping displays render black unless --stay-awake overrides them. */
    if ((!l->sleep_out || !l->disp_on) && !e->stay_awake) {
        for (int i = 0; i < PANEL_W * PANEL_H; i++) out[i] = 0xFF000000u;
        return;
    }
    for (int y = 0; y < PANEL_H; y++)
        for (int x = 0; x < PANEL_W; x++) {
            uint16_t p = l->gram[y][x];
            if (l->inverted) p = (uint16_t)~p;
            uint32_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
            uint32_t r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
            if (bgr) { uint32_t t = r; r = b; b = t; }
            out[y * PANEL_W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
}

int lcd_dump_bmp(Emu *e, const char *path)
{
    static uint32_t px[GRAM_W * GRAM_H];
    int w, h;
    lcd_render(e, px, &w, &h);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t imgsz = (uint32_t)(w * h * 4), off = 54, fsz = off + imgsz;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr+2,  &fsz, 4); memcpy(hdr+10, &off, 4);
    uint32_t bisz = 40; memcpy(hdr+14, &bisz, 4);
    int32_t bw = w, bh = -h;  /* negative = top-down */
    memcpy(hdr+18, &bw, 4); memcpy(hdr+22, &bh, 4);
    uint16_t planes = 1, bpp = 32;
    memcpy(hdr+26, &planes, 2); memcpy(hdr+28, &bpp, 2);
    memcpy(hdr+34, &imgsz, 4);
    fwrite(hdr, 1, 54, f);
    /* px is 0xAARRGGBB; BMP stores B, G, R, A. */
    for (int i = 0; i < w * h; i++) {
        uint8_t q[4] = { (uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16), 0xFF };
        fwrite(q, 1, 4, f);
    }
    fclose(f);
    return 0;
}

static void report_tbl(FILE *f, const char *name, const uint8_t *got, int gn,
                       const uint8_t *want, int wn)
{
    fprintf(f, "  %-4s: ", name);
    if (gn == 0) { fprintf(f, "never written\n"); return; }
    int match = (gn == wn) && !memcmp(got, want, (size_t)wn);
    for (int i = 0; i < gn; i++) fprintf(f, "%02x ", got[i]);
    fprintf(f, " -> %s\n", match ? "MATCH (iD)" : "differs from iD");
}

void lcd_report(Emu *e, FILE *f)
{
    Lcd *l = &e->lcd;
    fprintf(f, "[lcd] report: %llu commands, %llu RAMWR bytes, MADCTL=%02x COLMOD set=%d "
               "sleep_out=%d disp_on=%d\n",
            (unsigned long long)l->cmd_count, (unsigned long long)l->ramwr_bytes,
            l->madctl, l->colmod != 0, l->sleep_out, l->disp_on);
    fprintf(f, "[lcd] init stream vs expected iD (contrast patch) values:\n");
    static const uint8_t idc0[2] = {0x03, 0x00};
    static const uint8_t idc2[1] = {0x05}, idc5[1] = {0xC4}, idc6[1] = {0x13};
    report_tbl(f, "C0", l->pwr_c0, l->c0_n, idc0, 2);
    report_tbl(f, "C2", l->pwr_c2, l->c2_n, idc2, 1);
    report_tbl(f, "C5", l->vcom_c5, l->c5_n, idc5, 1);
    report_tbl(f, "C6", l->vcom_c6, l->c6_n, idc6, 1);
    report_tbl(f, "E0", l->gamma_pos, l->gp_n, ID_E0, 16);
    report_tbl(f, "E1", l->gamma_neg, l->gn_n, ID_E1, 16);
    if (l->gp_n == 16 && !memcmp(l->gamma_pos, PC_E0, 16))
        fprintf(f, "  (E0 matches PC stock gamma - unpatched init path)\n");
}
