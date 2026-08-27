#include "emu.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#define STATE_MAGIC "TAMASTAT"
#define STATE_VERSION 1u

typedef struct StateHeader {
    char magic[8], build[32], device[32];
    uint32_t version, emu_size, rom_size, flash_crc;
} StateHeader;

static void state_why(char *why, size_t whysz, const char *fmt, ...)
{
    va_list ap;
    if (!why || !whysz) return;
    va_start(ap, fmt);
    vsnprintf(why, whysz, fmt, ap);
    va_end(ap);
}

static uint32_t state_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
    }
    return ~crc;
}

static int state_path(char *out, size_t outsz, const char *savpath,
                      const char *suffix, char *why, size_t whysz)
{
    int n = snprintf(out, outsz, "%s%s", savpath, suffix);
    if (n < 0 || (size_t)n >= outsz) {
        state_why(why, whysz, "state path too long");
        return 0;
    }
    return 1;
}

static int state_name(char dst[32], const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= 32) return 0;
    memset(dst, 0, 32);
    if (n) memcpy(dst, src, n);
    return 1;
}

/* A save belongs to one Tamagotchi. Lock it while this window is open so a
 * second window cannot share its identity or overwrite its save/state files.
 * On Windows, the lock file disappears automatically if the emulator crashes. */
int state_sav_lock_acquire(const char *savpath, uintptr_t *token,
                           char *why, size_t whysz)
{
    char path[1024];
    if (!token || !state_path(path, sizeof path, savpath, ".lock", why, whysz))
        return 0;
    *token = 0;
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        state_why(why, whysz, "Another emulator instance is using\n%s", savpath);
        return 0;
    }
    *token = (uintptr_t)h;
#else
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) close(fd);
        state_why(why, whysz, "Another emulator instance is using\n%s", savpath);
        return 0;
    }
    *token = (uintptr_t)(fd + 1);
#endif
    state_why(why, whysz, "locked");
    return 1;
}

void state_sav_lock_release(uintptr_t *token)
{
    if (!token || !*token) return;
#ifdef _WIN32
    CloseHandle((HANDLE)*token);
#else
    close((int)(*token - 1));
#endif
    *token = 0;
}

/* The container validates its STAT record framing, CRC and tags. This shared
 * state.c path alone validates the native payload's build, sizes and flash CRC. */
StateResult state_decode(Emu *e, const uint8_t *blob, size_t blob_len,
                         const char *build_id, char *why, size_t whysz)
{
    char want_build[32], want_device[32];
    StateHeader h;
    Emu saved;

    if (!e || !e->rom || !blob || !build_id || !e->dev.name) {
        state_why(why, whysz, "invalid state decode arguments");
        return STATE_IO_ERROR;
    }
    if (!state_name(want_build, build_id) || !state_name(want_device, e->dev.name)) {
        state_why(why, whysz, "state build or device identifier too long");
        return STATE_IO_ERROR;
    }
    if (blob_len < sizeof h) { state_why(why, whysz, "truncated state header"); return STATE_REJECTED; }
    memcpy(&h, blob, sizeof h);
    if (memcmp(h.magic, STATE_MAGIC, 8)) {
        state_why(why, whysz, "state magic does not match"); return STATE_REJECTED;
    }
    if (h.version != STATE_VERSION) {
        state_why(why, whysz, "state version does not match"); return STATE_REJECTED;
    }
    if (h.emu_size != sizeof(Emu)) {
        state_why(why, whysz, "state Emu size does not match"); return STATE_REJECTED;
    }
    if (h.rom_size != e->dev.rom_size) {
        state_why(why, whysz, "state ROM size does not match"); return STATE_REJECTED;
    }
    if (memcmp(h.build, want_build, sizeof h.build)) {
        state_why(why, whysz, "state build identifier does not match"); return STATE_REJECTED;
    }
    if (memcmp(h.device, want_device, sizeof h.device)) {
        state_why(why, whysz, "state device name does not match"); return STATE_REJECTED;
    }
    if (h.flash_crc != state_crc32(e->rom, e->dev.rom_size)) {
        state_why(why, whysz, "state flash checksum does not match save"); return STATE_REJECTED;
    }
    if (blob_len < sizeof h + sizeof saved) { state_why(why, whysz, "truncated state payload"); return STATE_REJECTED; }
    if (blob_len != sizeof h + sizeof saved) { state_why(why, whysz, "state file has trailing data"); return STATE_REJECTED; }
    memcpy(&saved, blob + sizeof h, sizeof saved);

    {
        uint8_t *rom = e->rom;
        DeviceProfile dev = e->dev;
        bool trace = e->trace, io_log = e->io_log, ir_log = e->ir_log, tone_log = e->tone_log;
        bool debug_strap = e->debug_strap, stay_awake = e->stay_awake, no_sleep = e->no_sleep;
        bool btn_log = e->btn_log, nfc_log = e->nfc_log, nfc_log_rf = e->nfc_log_rf;
        bool auto_touch = e->auto_touch, ir_fast = e->ir_fast;
        bool lcd_log = e->lcd.log;
        bool auto_link = e->auto_link;
        int rtc_mult = e->rtc_mult, touch_type = e->touch_type, auto_link_port = e->auto_link_port;
        Link *link = e->link;
        Emu *nfc_peer = e->nfc_peer;
        NfcPeer *nfc_vpeer = e->nfc_vpeer;
        Link auto_link_storage = e->auto_link_storage;
        uint32_t watch_lo = e->watch_lo, watch_hi = e->watch_hi, fwatch_lo = e->fwatch_lo, fwatch_hi = e->fwatch_hi;
        int rwatch_n = e->rwatch_n, rwatch_budget = e->rwatch_budget, fwatch_budget = e->fwatch_budget;
        uint32_t rwatch_lo[4], rwatch_hi[4];
        uint32_t pc_hook[PC_HOOKS], call_lo[CALL_RANGES], call_hi[CALL_RANGES];
        const char *pc_hook_name[PC_HOOKS];
        uint8_t pc_hook_ring[PC_HOOKS];
        int n_pc_hook = e->n_pc_hook, call_nranges = e->call_nranges;
        memcpy(rwatch_lo, e->rwatch_lo, sizeof rwatch_lo); memcpy(rwatch_hi, e->rwatch_hi, sizeof rwatch_hi);
        memcpy(pc_hook, e->pc_hook, sizeof pc_hook); memcpy(pc_hook_name, e->pc_hook_name, sizeof pc_hook_name);
        memcpy(pc_hook_ring, e->pc_hook_ring, sizeof pc_hook_ring);
        memcpy(call_lo, e->call_lo, sizeof call_lo); memcpy(call_hi, e->call_hi, sizeof call_hi);
        *e = saved;
        e->rom = rom; e->dev = dev;
        e->trace = trace; e->io_log = io_log; e->ir_log = ir_log; e->tone_log = tone_log;
        e->debug_strap = debug_strap; e->stay_awake = stay_awake; e->no_sleep = no_sleep;
        e->btn_log = btn_log; e->nfc_log = nfc_log; e->nfc_log_rf = nfc_log_rf;
        e->auto_touch = auto_touch; e->ir_fast = ir_fast; e->rtc_mult = rtc_mult;
        e->lcd.log = lcd_log;
        e->touch_type = touch_type; e->auto_link = auto_link; e->auto_link_port = auto_link_port;
        e->link = link; e->nfc_peer = nfc_peer; e->nfc_vpeer = nfc_vpeer;
        e->auto_link_storage = auto_link_storage;
        e->watch_lo = watch_lo; e->watch_hi = watch_hi; e->fwatch_lo = fwatch_lo; e->fwatch_hi = fwatch_hi;
        e->rwatch_n = rwatch_n; e->rwatch_budget = rwatch_budget; e->fwatch_budget = fwatch_budget;
        memcpy(e->rwatch_lo, rwatch_lo, sizeof rwatch_lo); memcpy(e->rwatch_hi, rwatch_hi, sizeof rwatch_hi);
        e->n_pc_hook = n_pc_hook; e->call_nranges = call_nranges;
        memcpy(e->pc_hook, pc_hook, sizeof pc_hook); memcpy(e->pc_hook_name, pc_hook_name, sizeof pc_hook_name);
        memcpy(e->pc_hook_ring, pc_hook_ring, sizeof pc_hook_ring);
        memcpy(e->call_lo, call_lo, sizeof call_lo); memcpy(e->call_hi, call_hi, sizeof call_hi);
    }
    e->tracef = NULL;
    e->lcd.logf = NULL;
    state_why(why, whysz, "loaded");
    return STATE_LOADED;
}

StateResult state_load(Emu *e, const char *savpath, const char *build_id,
                       char *why, size_t whysz)
{
    char path[1024];
    FILE *f;
    long n;
    uint8_t *blob;
    StateResult result;

    if (!e || !e->rom || !savpath || !build_id || !e->dev.name) {
        state_why(why, whysz, "invalid state load arguments");
        return STATE_IO_ERROR;
    }
    if (!state_path(path, sizeof path, savpath, ".state", why, whysz)) return STATE_IO_ERROR;
    f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) { state_why(why, whysz, "no state file"); return STATE_NONE; }
        state_why(why, whysz, "cannot open state file: %s", strerror(errno));
        return STATE_IO_ERROR;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); state_why(why, whysz, "state read failed"); return STATE_IO_ERROR;
    }
    blob = malloc(n ? (size_t)n : 1);
    if (!blob) { fclose(f); state_why(why, whysz, "out of memory"); return STATE_IO_ERROR; }
    if ((size_t)n && fread(blob, 1, (size_t)n, f) != (size_t)n) {
        free(blob); fclose(f); state_why(why, whysz, "state read failed"); return STATE_IO_ERROR;
    }
    if (fclose(f) != 0) { free(blob); state_why(why, whysz, "state read failed"); return STATE_IO_ERROR; }
    result = state_decode(e, blob, (size_t)n, build_id, why, whysz);
    free(blob);
    return result;
}

int state_save(const Emu *e, const char *savpath, const char *build_id,
               char *why, size_t whysz)
{
    char path[1024], tmp[1024];
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    FILE *f = NULL;
    int ok = 0;
    if (!e || !e->rom || !savpath || !build_id || !e->dev.name) {
        state_why(why, whysz, "invalid state save arguments"); return 0;
    }
    if (!state_path(path, sizeof path, savpath, ".state", why, whysz) ||
        !state_path(tmp, sizeof tmp, savpath, ".state.tmp", why, whysz)) {
        state_why(why, whysz, "state build or device identifier too long"); return 0;
    }
    if (!state_encode(e, build_id, &blob, &blob_len, why, whysz)) return 0;
    f = fopen(tmp, "wb");
    if (!f) { free(blob); state_why(why, whysz, "cannot create state temporary file: %s", strerror(errno)); return 0; }
    if (fwrite(blob, 1, blob_len, f) != blob_len) {
        state_why(why, whysz, "cannot write state temporary file"); goto done;
    }
    if (fclose(f) != 0) { f = NULL; state_why(why, whysz, "cannot close state temporary file"); goto done; }
    f = NULL;
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        state_why(why, whysz, "cannot replace state file"); goto done;
    }
#else
    if (rename(tmp, path) != 0) { state_why(why, whysz, "cannot replace state file: %s", strerror(errno)); goto done; }
#endif
    ok = 1; state_why(why, whysz, "saved");
done:
    if (f) fclose(f);
    if (!ok) remove(tmp);
    free(blob);
    return ok;
}

int state_encode(const Emu *e, const char *build_id, uint8_t **blob, size_t *blob_len,
                 char *why, size_t whysz)
{
    StateHeader h;
    uint8_t *out;
    if (!e || !e->rom || !build_id || !blob || !blob_len || !e->dev.name ||
        !state_name(h.build, build_id) || !state_name(h.device, e->dev.name)) {
        state_why(why, whysz, "invalid state encode arguments"); return 0;
    }
    out = malloc(sizeof h + sizeof *e);
    if (!out) { state_why(why, whysz, "out of memory"); return 0; }
    memcpy(h.magic, STATE_MAGIC, 8); h.version = STATE_VERSION;
    h.emu_size = sizeof(Emu); h.rom_size = e->dev.rom_size;
    h.flash_crc = state_crc32(e->rom, e->dev.rom_size);
    memcpy(out, &h, sizeof h); memcpy(out + sizeof h, e, sizeof *e);
    *blob = out; *blob_len = sizeof h + sizeof *e; state_why(why, whysz, "encoded");
    return 1;
}
