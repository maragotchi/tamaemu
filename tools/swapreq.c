
#include "swapreq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sum32(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int swapreq_write(const char *path, const SwapWrite *w, int count)
{
    if (count <= 0 || count > SWAPREQ_MAX) return -1;
    size_t body = (size_t)count * 12;
    for (int i = 0; i < count; i++) if (w[i].bytes) body += w[i].len;

    uint8_t *buf = malloc(20 + body);
    if (!buf) return -1;
    memcpy(buf, SWAPREQ_MAGIC, 8);
    put32(buf + 8, SWAPREQ_VERSION);
    put32(buf + 12, (uint32_t)count);

    uint8_t *e = buf + 20;
    for (int i = 0; i < count; i++) {
        put32(e, w[i].off); put32(e + 4, w[i].len);
        put32(e + 8, w[i].bytes ? 0u : 1u);
        e += 12;
    }
    for (int i = 0; i < count; i++)
        if (w[i].bytes) { memcpy(e, w[i].bytes, w[i].len); e += w[i].len; }

    put32(buf + 16, sum32(buf + 20, body));

    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); return -1; }
    size_t wrote = fwrite(buf, 1, 20 + body, f);
    fclose(f);
    free(buf);
    if (wrote != 20 + body) { remove(tmp); return -1; }
    /* rename() cannot replace files on Windows. */
    remove(path);
    return rename(tmp, path) == 0 ? 0 : -1;
}

int swapreq_read(const char *path, SwapReq *r)
{
    memset(r, 0, sizeof *r);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    rewind(f);
    if (sz < 20) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    if (memcmp(buf, SWAPREQ_MAGIC, 8) != 0 || get32(buf + 8) != SWAPREQ_VERSION) {
        free(buf); return -1;
    }
    uint32_t count = get32(buf + 12);
    if (count == 0 || count > SWAPREQ_MAX) { free(buf); return -1; }

    size_t need = 20 + (size_t)count * 12;
    if ((size_t)sz < need) { free(buf); return -1; }
    if (get32(buf + 16) != sum32(buf + 20, (size_t)sz - 20)) { free(buf); return -1; }

    const uint8_t *e = buf + 20;
    const uint8_t *pay = e + (size_t)count * 12;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = get32(e), len = get32(e + 4), fill = get32(e + 8);
        e += 12;
        r->w[i].off = off; r->w[i].len = len;
        if (fill) { r->w[i].bytes = NULL; continue; }
        if ((size_t)(pay - buf) + len > (size_t)sz) { free(buf); return -1; }
        r->w[i].bytes = pay;
        pay += len;
    }
    r->count = (int)count;
    r->owned = buf;
    return 0;
}

void swapreq_free(SwapReq *r)
{
    free(r->owned);
    memset(r, 0, sizeof *r);
}
