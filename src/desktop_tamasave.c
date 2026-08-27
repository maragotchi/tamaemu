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
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
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

static int verify_container_file(const char *path, char *why, size_t whysz)
{
    TamaSave verified = {0};
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    int ok = read_file(path, &wire, &wire_len, why, whysz) &&
             tamasave_decode(wire, wire_len, &verified, why, whysz);
    tamasave_free(&verified);
    free(wire);
    return ok;
}

enum { META_LENGTH = 44 };

static int metadata_path(char *out, size_t outsz, const char *savpath,
                         char *why, size_t whysz)
{
    return bundle_path(out, outsz, savpath, ".tamasave.meta", why, whysz);
}

static int new_lineage(uint8_t lineage[16], char *why, size_t whysz)
{
#ifdef _WIN32
    if (BCryptGenRandom(NULL, lineage, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return 1;
    desktop_why(why, whysz, "cannot obtain random cross save lineage");
    return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd < 0) {
        desktop_why(why, whysz, "cannot obtain random cross save lineage");
        return 0;
    }
    while (got < 16) {
        ssize_t n = read(fd, lineage + got, 16 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got == 16) return 1;
    desktop_why(why, whysz, "cannot obtain random cross save lineage");
    return 0;
#endif
}

/* This is the transition-only metadata reader. A container is authoritative
 * when present; the retired sidecar is considered only while it is absent. */
static int read_metadata(const char *savpath, TamaSave *meta,
                         char *why, size_t whysz)
{
    char path[1200];
    uint8_t wire[META_LENGTH];
    FILE *f;
    int valid;

    {
        TamaSave embedded = {0};
        uint8_t *container = NULL;
        size_t container_len = 0;
        if (!bundle_path(path, sizeof path, savpath, ".tamasave", why, whysz)) return -1;
        if (read_file(path, &container, &container_len, why, whysz)) {
            if (!tamasave_decode(container, container_len, &embedded, why, whysz)) {
                /* An unreadable container has no usable lineage. It stays on disk
                 * until a later verified session write replaces it. */
                free(container);
                return 0;
            }
            memcpy(meta->lineage, embedded.lineage, sizeof meta->lineage);
            meta->revision = embedded.revision;
            meta->saved_utc_ms = embedded.saved_utc_ms;
            tamasave_free(&embedded); free(container);
            return 1;
        }
        if (errno != ENOENT) return -1;
    }
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
        desktop_why(why, whysz, "invalid cross save metadata for %s", savpath);
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

/* Migration is deliberately delete-after-verified-write: an interrupted export must
 * leave every recoverable legacy sidecar in place. */
static void remove_legacy_sidecars(const char *savpath)
{
    char path[1200];
    const char *suffixes[] = { ".ram", ".state", ".tamasave.meta" };
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++)
        if (bundle_path(path, sizeof path, savpath, suffixes[i], NULL, 0)) remove(path);
}

int desktop_write_tamasave(const char *savpath, const char *out,
                           const char *device,
                           const uint8_t *sav, size_t sav_len,
                           const uint8_t *ram, size_t ram_len,
                           const uint8_t *state, size_t state_len,
                           const char *state_build,
                           char *why, size_t whysz)
{
    TamaSave save = {0}, previous = {0};
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    int had_metadata;
    int ok = 0;

    if (!savpath || !out || !device || !*device || !sav || !sav_len || !ram || !ram_len ||
        (state_len && (!state || !state_build || !*state_build))) {
        desktop_why(why, whysz, "invalid cross save write arguments"); return 0;
    }
    had_metadata = read_metadata(savpath, &previous, why, whysz);
    if (had_metadata < 0) goto done;
    if (had_metadata) {
        memcpy(save.lineage, previous.lineage, sizeof save.lineage);
        save.revision = previous.revision;
    } else if (!new_lineage(save.lineage, why, whysz)) goto done;
    if (save.revision == UINT64_MAX) {
        desktop_why(why, whysz, "cross save revision is exhausted"); goto done;
    }
    save.revision++;
    save.saved_utc_ms = (uint64_t)time(NULL) * 1000;
    save.sav = (uint8_t *)sav; save.sav_len = sav_len;
    save.ram = (uint8_t *)ram; save.ram_len = ram_len;
    strncpy(save.device, device, sizeof save.device - 1);
    if (state_len) {
        save.state = (uint8_t *)state; save.state_len = state_len;
        strcpy(save.state_runtime, "tamaemu");
        strncpy(save.state_build, state_build, sizeof save.state_build - 1);
        save.state_version = 1;
    }
    /* Container code verifies record framing/CRC/tags; state.c alone validates
     * the native payload's build, sizes, and flash checksum. */
    if (!tamasave_encode(&save, &wire, &wire_len, why, whysz) ||
        !write_atomic(out, wire, wire_len, why, whysz) ||
        !verify_container_file(out, why, whysz))
        goto done;
    /* Migration is deliberately delete-after-verified-write: an interrupted
     * container write must leave every recoverable sidecar in place. */
    if (strlen(out) == strlen(savpath) + 9 && !strcmp(out + strlen(savpath), ".tamasave"))
        remove_legacy_sidecars(savpath);
    desktop_why(why, whysz, "saved");
    ok = 1;
done:
    free(wire);
    tamasave_free(&previous);
    return ok;
}

int desktop_export_tamasave(const char *savpath, const char *out,
                            const char *device, char *why, size_t whysz)
{
    if (!savpath || !out || !device || !*device) {
        desktop_why(why, whysz, "invalid export arguments"); return 0;
    }
    {
        char current[1024];
        uint8_t *current_wire = NULL;
        size_t current_len = 0;
        TamaSave verified = {0};
        /* Export is a copy, not a new save point: keep META revision intact. */
        if (bundle_path(current, sizeof current, savpath, ".tamasave", why, whysz) &&
            strcmp(current, out) && read_file(current, &current_wire, &current_len, why, whysz)) {
            int copied = tamasave_decode(current_wire, current_len, &verified, why, whysz) &&
                         write_atomic(out, current_wire, current_len, why, whysz) &&
                         verify_container_file(out, why, whysz);
            tamasave_free(&verified); free(current_wire);
            if (copied) { desktop_why(why, whysz, "exported"); return 1; }
            return 0;
        }
        free(current_wire);
        if (!strcmp(current, out)) { desktop_why(why, whysz, "export target is already current cross save"); return 0; }
        if (errno == ENOENT) { desktop_why(why, whysz, "no current cross save to export"); return 0; }
        return 0;
    }
}

int desktop_import_tamasave(const char *in, const char *savpath,
                            const char *device, size_t flash_len, int force,
                            char *why, size_t whysz)
{
    TamaSave save = {0}, current = {0};
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    uintptr_t lock = 0;
    char container_path[1024];
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
                        "incoming cross save revision is not newer; use --force-tamasave-import to replace it");
            goto done;
        }
    }
    if (!bundle_path(container_path, sizeof container_path, savpath, ".tamasave", why, whysz) ||
        !write_atomic(savpath, save.sav, save.sav_len, why, whysz) ||
        !write_atomic(container_path, wire, wire_len, why, whysz))
        goto done;
    desktop_why(why, whysz, "imported");
    ok = 1;
done:
    state_sav_lock_release(&lock);
    free(wire);
    tamasave_free(&save);
    return ok;
}

int desktop_restore_tamasave(const char *savpath, Emu *e, const char *build_id,
                             int restore_state, StateResult *state_result,
                             char *why, size_t whysz)
{
    char path[1200]; uint8_t *wire = NULL; size_t wire_len = 0; TamaSave save = {0};
    if (state_result) *state_result = STATE_NONE;
    if (!bundle_path(path, sizeof path, savpath, ".tamasave", why, whysz)) return -1;
    if (!read_file(path, &wire, &wire_len, why, whysz)) return errno == ENOENT ? 0 : -1;
    if (!tamasave_decode(wire, wire_len, &save, why, whysz)) { free(wire); return -1; }
    if (save.ram_len != e->dev.a0ram_size) {
        desktop_why(why, whysz, "container RAM size does not match device"); tamasave_free(&save); free(wire); return -1;
    }
    memcpy(e->a0ram, save.ram, save.ram_len);
    if (restore_state && save.state && !strcmp(save.state_runtime, "tamaemu")) {
        StateResult result = state_decode(e, save.state, save.state_len, build_id, why, whysz);
        if (state_result) *state_result = result;
    }
    tamasave_free(&save); free(wire); return 1;
}
