#include "tamasave.h"
#include "desktop_tamasave.h"
#include "emu.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static void desktop_why(char *why, size_t whysz, const char *fmt, ...)
{
    va_list ap;
    if (!why || !whysz) return;
    va_start(ap, fmt);
    vsnprintf(why, whysz, fmt, ap);
    va_end(ap);
}

static int bundle_path(char *out, size_t outsz, const char *savpath,
                       const char *suffix, char *why, size_t whysz)
{
    int n = snprintf(out, outsz, "%s%s", savpath, suffix);
    if (n < 0 || (size_t)n >= outsz) {
        desktop_why(why, whysz, "save path is too long");
        return 0;
    }
    return 1;
}

static int read_file(const char *path, uint8_t **data, size_t *len,
                     char *why, size_t whysz)
{
    FILE *f;
    long n;
    uint8_t *p;

    *data = NULL; *len = 0;
    f = fopen(path, "rb");
    if (!f) { desktop_why(why, whysz, "cannot open %s: %s", path, strerror(errno)); return 0; }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); desktop_why(why, whysz, "cannot read %s", path); return 0;
    }
    p = malloc(n ? (size_t)n : 1);
    if (!p) { fclose(f); desktop_why(why, whysz, "out of memory"); return 0; }
    if ((size_t)n && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); desktop_why(why, whysz, "cannot read %s", path); return 0;
    }
    if (fclose(f) != 0) { free(p); desktop_why(why, whysz, "cannot close %s", path); return 0; }
    *data = p; *len = (size_t)n;
    return 1;
}

static int write_atomic(const char *path, const uint8_t *data, size_t len,
                        char *why, size_t whysz)
{
    char tmp[1200];
    FILE *f;
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        desktop_why(why, whysz, "save path is too long"); return 0;
    }
    f = fopen(tmp, "wb");
    if (!f) { desktop_why(why, whysz, "cannot create %s: %s", tmp, strerror(errno)); return 0; }
    if ((len && fwrite(data, 1, len, f) != len) || fclose(f) != 0) {
        remove(tmp); desktop_why(why, whysz, "cannot write %s", tmp); return 0;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp); desktop_why(why, whysz, "cannot replace %s", path); return 0;
    }
#else
    if (rename(tmp, path) != 0) {
        remove(tmp); desktop_why(why, whysz, "cannot replace %s: %s", path, strerror(errno)); return 0;
    }
#endif
    return 1;
}

enum { META_LENGTH = 44 };

static int metadata_path(char *out, size_t outsz, const char *savpath,
                         char *why, size_t whysz)
{
    return bundle_path(out, outsz, savpath, ".tamasave.meta", why, whysz);
}

static void new_lineage(uint8_t lineage[16])
{
    static uint64_t serial;
    uint64_t a = (uint64_t)time(NULL) ^ (uintptr_t)lineage;
    uint64_t b = ++serial ^ (a << 17) ^ (a >> 11);
    for (int i = 0; i < 8; i++) {
        lineage[i] = (uint8_t)(a >> (i * 8));
        lineage[i + 8] = (uint8_t)(b >> (i * 8));
    }
}

/* Desktop has no slot database, so this sidecar remembers the lineage and
 * revision beside the raw save. That lets stale checks follow the same pet
 * after desktop play without putting desktop-only details in .tamasave. */
static int read_metadata(const char *savpath, TamaSave *meta,
                         char *why, size_t whysz)
{
    char path[1200];
    uint8_t wire[META_LENGTH];
    FILE *f;
    int valid;

    if (!metadata_path(path, sizeof path, savpath, why, whysz)) return -1;
    f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 0;
        desktop_why(why, whysz, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    valid = fread(wire, 1, sizeof wire, f) == sizeof wire && fgetc(f) == EOF;
    if (fclose(f) != 0) valid = 0;
    if (!valid || memcmp(wire, "TAMAMETA", 8) || wire[8] != 1 || wire[9] ||
        wire[10] || wire[11]) {
        desktop_why(why, whysz, "invalid in-progress save metadata for %s", savpath);
        return -1;
    }
    memset(meta, 0, sizeof *meta);
    memcpy(meta->lineage, wire + 12, sizeof meta->lineage);
    for (int i = 0; i < 8; i++) {
        meta->revision |= (uint64_t)wire[28 + i] << (i * 8);
        meta->saved_utc_ms |= (uint64_t)wire[36 + i] << (i * 8);
    }
    return 1;
}

static int write_metadata(const char *savpath, const TamaSave *meta,
                          char *why, size_t whysz)
{
    char path[1200];
    uint8_t wire[META_LENGTH] = { 0 };

    if (!metadata_path(path, sizeof path, savpath, why, whysz)) return 0;
    memcpy(wire, "TAMAMETA", 8);
    wire[8] = 1;
    memcpy(wire + 12, meta->lineage, sizeof meta->lineage);
    for (int i = 0; i < 8; i++) {
        wire[28 + i] = (uint8_t)(meta->revision >> (i * 8));
        wire[36 + i] = (uint8_t)(meta->saved_utc_ms >> (i * 8));
    }
    return write_atomic(path, wire, sizeof wire, why, whysz);
}

int desktop_export_tamasave(const char *savpath, const char *out,
                            const char *device, char *why, size_t whysz)
{
    TamaSave save = {0}, previous = {0};
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    char ram_path[1024], state_path[1024];
    int ok = 0;

    if (!savpath || !out || !device || !*device) {
        desktop_why(why, whysz, "invalid export arguments"); return 0;
    }
    {
        int had_metadata = read_metadata(savpath, &previous, why, whysz);
        if (had_metadata < 0) goto done;
        if (had_metadata) {
            memcpy(save.lineage, previous.lineage, sizeof save.lineage);
            save.revision = previous.revision;
        } else new_lineage(save.lineage);
        if (save.revision == UINT64_MAX) {
            desktop_why(why, whysz, "in-progress save revision is exhausted"); goto done;
        }
        save.revision++;
        save.saved_utc_ms = (uint64_t)time(NULL) * 1000;
    }
    if (!bundle_path(ram_path, sizeof ram_path, savpath, ".ram", why, whysz) ||
        !bundle_path(state_path, sizeof state_path, savpath, ".state", why, whysz) ||
        !read_file(savpath, &save.sav, &save.sav_len, why, whysz) ||
        !read_file(ram_path, &save.ram, &save.ram_len, why, whysz))
        goto done;
    if (!read_file(state_path, &save.state, &save.state_len, why, whysz)) {
        /* A session snapshot is optional; other state-file failures are not. */
        if (errno != ENOENT) goto done;
        save.state = NULL; save.state_len = 0;
    }
    strncpy(save.device, device, sizeof save.device - 1);
    if (save.state) {
        strcpy(save.state_runtime, "tamaemu");
        strcpy(save.state_build, "native");
        save.state_version = 1;
    }
    if (!tamasave_encode(&save, &wire, &wire_len, why, whysz) ||
        !write_atomic(out, wire, wire_len, why, whysz) ||
        !write_metadata(savpath, &save, why, whysz))
        goto done;
    desktop_why(why, whysz, "exported");
    ok = 1;
done:
    free(wire);
    tamasave_free(&save);
    return ok;
}

int desktop_import_tamasave(const char *in, const char *savpath,
                            const char *device, size_t flash_len, int force,
                            char *why, size_t whysz)
{
    TamaSave save = {0}, current = {0};
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    uintptr_t lock = 0;
    char ram_path[1024], state_path[1024];
    int ok = 0;

    if (!in || !savpath || !device || !*device || !flash_len) {
        desktop_why(why, whysz, "invalid import arguments"); return 0;
    }
    /* Take the normal per-save lock before reading or replacing any bundle file. */
    if (!state_sav_lock_acquire(savpath, &lock, why, whysz)) return 0;
    if (!read_file(in, &wire, &wire_len, why, whysz) ||
        !tamasave_decode(wire, wire_len, &save, why, whysz))
        goto done;
    if (strcmp(save.device, device) != 0) {
        desktop_why(why, whysz, "container is for %s, not %s", save.device, device); goto done;
    }
    if (save.sav_len != flash_len) {
        desktop_why(why, whysz, "container flash size does not match selected device"); goto done;
    }
    {
        int had_metadata = read_metadata(savpath, &current, why, whysz);
        if (had_metadata < 0) goto done;
        if (had_metadata && !memcmp(current.lineage, save.lineage, sizeof save.lineage) &&
            save.revision <= current.revision && !force) {
            desktop_why(why, whysz,
                        "incoming in-progress save revision is not newer; use --force-tamasave-import to replace it");
            goto done;
        }
    }
    if (!bundle_path(ram_path, sizeof ram_path, savpath, ".ram", why, whysz) ||
        !bundle_path(state_path, sizeof state_path, savpath, ".state", why, whysz) ||
        !write_atomic(savpath, save.sav, save.sav_len, why, whysz) ||
        !write_atomic(ram_path, save.ram, save.ram_len, why, whysz) ||
        !write_metadata(savpath, &save, why, whysz))
        goto done;
    /* A browser snapshot is not a native state file. Keep only the snapshot
     * this runtime owns; state_load performs the remaining build validation. */
    if (save.state && !strcmp(save.state_runtime, "tamaemu") &&
        !write_atomic(state_path, save.state, save.state_len, why, whysz))
        goto done;
    desktop_why(why, whysz, "imported");
    ok = 1;
done:
    state_sav_lock_release(&lock);
    free(wire);
    tamasave_free(&save);
    return ok;
}
