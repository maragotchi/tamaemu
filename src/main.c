#include "emu.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#endif

/* Create snaps/ once and report failed writes. */
static void snap_dump(Emu *e, const char *path)
{
    static int tried;
    if (!tried) { tried = 1; MKDIR("snaps"); }
    if (lcd_dump_bmp(e, path) != 0)
        fprintf(stderr, "[snap] FAILED to write %s\n", path);
}

#ifdef USE_SDL
#include <SDL2/SDL.h>
#include <timeapi.h>
#endif

static struct { uint32_t addr; const char *label; bool hit; } checkpoints[] = {
    {0x024FDFE6, "reset handler (CMU init)", false},
    {0x024017CE, "LCD init variant A", false},
    {0x0240A5BE, "LCD init variant B", false},
};
#define NCHECK (sizeof checkpoints / sizeof checkpoints[0])

/* Snapshots only load in the build that wrote them. */
static const char snapshot_build_id[] = __DATE__ " " __TIME__;

/* Log scanner arguments at entry, after the caller's delay slot has run. */
static bool scanlog;
static void scan_hook(Emu *e)
{
    static const struct { uint32_t pc; const char *name; } hooks[] = {
        {0x0251FDC2, "find-free-slot"},
        {0x0251FE04, "find-item-kind"},
        {0x0251FD38, "scan-A"},
        {0x0251FD6E, "scan-B"},
        {0x0251FC76, "slot-check"},
        {0x0251F3CE, "kind-slot-addr"},
    };
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++)
        if (e->pc == hooks[i].pc)
            fprintf(stderr, "[scan] %s r6=%08x r7=%08x r8=%08x r9=%08x cyc=%llu\n",
                    hooks[i].name, e->r[6], e->r[7], e->r[8], e->r[9],
                    (unsigned long long)e->cycles);
}

/* --flight writes executed PCs as little-endian uint32_t values. */
static FILE *flightf;
static long flight_left;
static uint32_t flight_trigger;
static bool flight_armed;

static void flight_hook(Emu *e)
{
    if (flight_armed && e->pc == flight_trigger && e->r[6] == 7) {
        flight_armed = false;
        fprintf(stderr, "[flight] trigger at pc=%08x cyc=%llu - recording\n",
                e->pc, (unsigned long long)e->cycles);
    }
    if (!flight_armed && flight_left > 0 && flightf) {
        fwrite(&e->pc, 4, 1, flightf);
        if (--flight_left == 0) {
            fclose(flightf); flightf = NULL;
            fprintf(stderr, "[flight] recording complete (flight.bin)\n");
        }
    }
}

static void check_pc(Emu *e)
{
    if (flightf) flight_hook(e);
    if (scanlog) scan_hook(e);
    /* At the exchange return, the stack still holds the caller address. */
    if (e->ir_log && e->pc == 0x0251ACF8u) {
        static uint32_t seen[8]; static int n;
        uint32_t ra = mem_read32(e, e->sp);
        int k = 0; for (; k < n; k++) if (seen[k] == ra) break;
        if (k == n && n < 8) { seen[n++] = ra;
            fprintf(stderr, "[ircall] exchange fn returns to %08x\n", ra); }
    }
    /* Firmware result bytes are at ctx+0x24/+0x25; 0xAA means aborted. */
    if (e->ir_log && e->pc == 0x0251ACCAu) {
        uint32_t ctx = mem_read32(e, 0x842F4u);
        if (ctx > 0x1000u) {
            uint32_t magic = mem_read32(e, ctx);   /* fw wants 'TAMA' (0x54414D41) */
            fprintf(stderr, "[irres] exchange exit: ctx=%08x magic=%08x%s "
                            "result=%02x/%02x t=%.2fs\n",
                    ctx, magic, magic == 0x54414D41u ? " (TAMA ok)" : " (BAD MAGIC)",
                    mem_read8(e, ctx + 0x24u), mem_read8(e, ctx + 0x25u), e->emu_secs);
        }
    }
    if (e->pc < 0x02400000) return;
    for (size_t i = 0; i < NCHECK; i++)
        if (!checkpoints[i].hit && checkpoints[i].addr == e->pc) {
            checkpoints[i].hit = true;
            fprintf(stderr, "[checkpoint] %08x %s (cyc=%llu)\n",
                    e->pc, checkpoints[i].label, (unsigned long long)e->cycles);
        }
}

static void spin_report(Emu *e)
{
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t p = e->trace_ring[i];
        if (!p) continue;
        if (p < lo) lo = p;
        if (p > hi) hi = p;
    }
    fprintf(stderr, "[spin] cyc=%llu pc window %08x..%08x, last IO read %08x, "
            "lcd_writes=%llu io_writes=%llu halted=%d\n",
            (unsigned long long)e->cycles, lo, hi, e->last_io_read,
            (unsigned long long)e->lcd_writes, (unsigned long long)e->io_writes, e->halted);
    char d[96];
    for (uint32_t p = lo; p <= hi && p < lo + 40; ) {
        int n = disasm_one(e, p, d, sizeof d);
        fprintf(stderr, "[spin]   %08x  %s\n", p, d);
        p += (uint32_t)n;
    }
}

static void usage(void)
{
    fprintf(stderr,
      "tamaemu <rom.bin> [--headless] [--max-cycles N] [--dump-bmp out.bmp]\n"
      "        [--link romB.bin [--sav-b b.sav] [--device-b NAME  core B's own profile]]\n"
      "        [--trace[-from N]] [--io-log] [--lcd-log] [--osc3 HZ] [--turbo]\n"
      "        [--device NAME  (--device help lists them; default ps)]\n"
      "        [--disasm lo hi] [--no-buttons] [--press cyc:mask:dur[,..]]\n"
      "        [--keys ABC  rebind the A/B/C keys (a-z/1-9; s/n reserved)]\n"
      "        [--nfc-log  decoded PN512 traffic on the 4U family]\n"
      "        [--nfc-probe CYC  a store NFC point in the field from cycle CYC]\n"
      "SDL keys: Z/Left=A  X/Down=B  C/Right=C (--keys rebinds the letters)\n"
      "          S=toggle stay-awake  ESC=quit\n"
      "          N=touch/untouch a store NFC point (bingo/gashapon/touch spots)\n"
      "          +/- step the game clock (1 2 5 10 30 60 120 300 600), 0=real time\n"
      "        [--stay-awake starts it on: screen never sleeps, tama keeps animating]\n"
      "        [--on-top  keep the window above other windows]\n"
      "        [--persist-ram  keep A0RAM in <sav>.ram so the tama survives a restart]\n"
      "        [--restart  cold boot, ignoring any saved machine snapshot]\n"
      "        [--no-state  do not load or write a machine snapshot]\n");
    exit(1);
}

/* Scripted press format: cyc:mask:duration_cycles. */
static struct { uint64_t at, until; uint8_t mask; } presses[32];
static int n_presses;

static void parse_presses(const char *s)
{
    while (*s && n_presses < 32) {
        uint64_t at = strtoull(s, (char **)&s, 0);
        if (*s++ != ':') break;
        uint8_t mask = (uint8_t)strtoul(s, (char **)&s, 0);
        if (*s++ != ':') break;
        uint64_t dur = strtoull(s, (char **)&s, 0);
        presses[n_presses].at = at;
        presses[n_presses].until = at + dur;
        presses[n_presses].mask = mask;
        n_presses++;
        if (*s == ',') s++;
    }
}

static uint8_t scripted_mask(uint64_t cyc)
{
    uint8_t m = 0;
    for (int i = 0; i < n_presses; i++)
        if (cyc >= presses[i].at && cyc < presses[i].until) m |= presses[i].mask;
    return m;
}

/* --press-b scripts core B during headless link runs. */
static struct { uint64_t at, until; uint8_t mask; } presses_b[32];
static int n_presses_b;

static void parse_presses_b(const char *s)
{
    while (*s && n_presses_b < 32) {
        uint64_t at = strtoull(s, (char **)&s, 0);
        if (*s++ != ':') break;
        uint8_t mask = (uint8_t)strtoul(s, (char **)&s, 0);
        if (*s++ != ':') break;
        uint64_t dur = strtoull(s, (char **)&s, 0);
        presses_b[n_presses_b].at = at;
        presses_b[n_presses_b].until = at + dur;
        presses_b[n_presses_b].mask = mask;
        n_presses_b++;
        if (*s == ',') s++;
    }
}

static uint8_t scripted_mask_b(uint64_t cyc)
{
    uint8_t m = 0;
    for (int i = 0; i < n_presses_b; i++)
        if (cyc >= presses_b[i].at && cyc < presses_b[i].until) m |= presses_b[i].mask;
    return m;
}

/* Core B inherits core A's profile unless --device-b overrides it. */
static int load_core(Emu *e, const DeviceProfile *dev, const char *rompath,
                     const char *savpath, bool persist_ram)
{
    e->dev = *dev;
    e->cmu.osc3_hz = dev->osc3_hz;
    e->rom = calloc(1, e->dev.rom_size);
    FILE *f = fopen(rompath, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", rompath); return 0; }
    fread(e->rom, 1, e->dev.rom_size, f); fclose(f);
    fprintf(stderr, "[rom] core-B %s loaded\n", rompath);
    if (savpath) {
        FILE *sf = fopen(savpath, "rb");
        if (sf) { fread(e->rom, 1, e->dev.rom_size, sf); fclose(sf);
                  fprintf(stderr, "[flash] core-B save %s loaded\n", savpath); }
    }
    if (persist_ram && savpath) {
        char rp[1088];
        snprintf(rp, sizeof rp, "%s.ram", savpath);
        FILE *rf = fopen(rp, "rb");
        if (rf) {
            size_t rn = fread(e->a0ram, 1, e->dev.a0ram_size, rf);
            fclose(rf);
            fprintf(stderr, "[ram] core-B restored %zu bytes from %s\n", rn, rp);
        } else {
            fprintf(stderr, "[ram] core-B: no %s yet - starting cold\n", rp);
        }
    }
    cpu_reset(e);
    fprintf(stderr, "[cpu] core-B reset vector -> %08x\n", e->pc);
    return 1;
}

/* Persist one linked core, writing flash only when dirty. */
static void core_persist(Emu *e, const char *savpath, const char *which)
{
    if (!savpath) return;
    if (e->flash_dirty) {
        FILE *of = fopen(savpath, "wb");
        if (of) {
            fwrite(e->rom, 1, e->dev.rom_size, of);
            fclose(of);
            fprintf(stderr, "[flash] core-%s %llu programs, %llu erases -> saved %s\n",
                    which, (unsigned long long)e->flash_programs,
                    (unsigned long long)e->flash_erases, savpath);
        } else fprintf(stderr, "[flash] core-%s cannot write %s\n", which, savpath);
    }
    char rp[1088];
    snprintf(rp, sizeof rp, "%s.ram", savpath);
    FILE *rf = fopen(rp, "wb");
    if (rf) {
        fwrite(e->a0ram, 1, e->dev.a0ram_size, rf);
        fclose(rf);
        fprintf(stderr, "[ram] core-%s saved %u bytes -> %s\n",
                which, (unsigned)e->dev.a0ram_size, rp);
    } else fprintf(stderr, "[ram] core-%s cannot write %s\n", which, rp);
}

/* Leave the old save alone until the replacement is fully written and closed. */
static int flash_save_atomic(const char *savpath, const uint8_t *rom, size_t len)
{
    char tmp[1088];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", savpath);
    if (n < 0 || (size_t)n >= sizeof tmp) return 0;
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    size_t wrote = fwrite(rom, 1, len, f);
    int close_rc = fclose(f);
    if (wrote != len || close_rc != 0) {
        remove(tmp);
        return 0;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, savpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        return 0;
    }
#else
    if (rename(tmp, savpath) != 0) {
        remove(tmp);
        return 0;
    }
#endif
    return 1;
}

/* A ring-enabled PC hook also dumps the preceding 64 instructions. */
static void add_pc_hook_ex(Emu *e, uint32_t addr, const char *name, int ring)
{
    if (e->n_pc_hook >= PC_HOOKS) { fprintf(stderr, "[pc] too many hooks\n"); return; }
    e->pc_hook[e->n_pc_hook] = addr;
    e->pc_hook_name[e->n_pc_hook] = name;
    e->pc_hook_ring[e->n_pc_hook] = (uint8_t)ring;
    e->n_pc_hook++;
    fprintf(stderr, "[pc] hook %s at %08x%s\n", name, addr, ring ? " (+trace ring)" : "");
}

static void add_pc_hook(Emu *e, uint32_t addr, const char *name)
{
    add_pc_hook_ex(e, addr, name, 0);
}

#ifdef USE_SDL
/* Keep linked cores at one game-clock rate and show it in the window title. */
static void set_speed(Emu *a, Emu *b, int mult, SDL_Window *w, const char *base)
{
    a->rtc_mult = mult;
    if (b) b->rtc_mult = mult;
    fprintf(stderr, "[speed] game clock x%d\n", mult);
    char t[80];
    if (mult > 1) snprintf(t, sizeof t, "%s -- %dx", base, mult);
    else          snprintf(t, sizeof t, "%s", base);
    SDL_SetWindowTitle(w, t);
}
#endif

/* Swap requests are scoped to one save to prevent cross-instance pickup. */
static uint32_t swap_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Returns writes made, or -1 without modifying ROM. */
static int swap_apply(Emu *e, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 20) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    int ok = memcmp(buf, "TAMASWAP", 8) == 0 && swap_get32(buf + 8) == 1u;
    uint32_t count = ok ? swap_get32(buf + 12) : 0;
    if (!ok || count == 0 || count > 8 || (size_t)sz < 20 + (size_t)count * 12) {
        free(buf); return -1;
    }
    uint32_t sum = 0;
    for (long i = 20; i < sz; i++) sum += buf[i];
    if (sum != swap_get32(buf + 16)) { free(buf); return -1; }

    /* Pass 1: check every entry fits before touching anything. */
    const uint8_t *e0 = buf + 20;
    const uint8_t *pay = e0 + (size_t)count * 12;
    const uint8_t *p = pay;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = swap_get32(e0 + i * 12), len = swap_get32(e0 + i * 12 + 4);
        uint32_t fill = swap_get32(e0 + i * 12 + 8);
        if ((size_t)off + len > e->dev.rom_size) { free(buf); return -1; }
        if (!fill) {
            if ((size_t)(p - buf) + len > (size_t)sz) { free(buf); return -1; }
            p += len;
        }
    }
    /* Pass 2: apply. */
    p = pay;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = swap_get32(e0 + i * 12), len = swap_get32(e0 + i * 12 + 4);
        uint32_t fill = swap_get32(e0 + i * 12 + 8);
        if (fill) memset(e->rom + off, 0xFF, len);
        else { memcpy(e->rom + off, p, len); p += len; }
        fprintf(stderr, "[swap] %s %08x +%u\n", fill ? "erase" : "write", off, len);
    }
    e->flash_dirty = true;
    free(buf);
    remove(path);
    return (int)count;
}

int main(int argc, char **argv)
{
    static Emu e;
    const char *rompath = NULL, *bmp = NULL, *savoverride = NULL, *nfc_inject = NULL;
    bool persist_ram = false;
    const char *linkrom = NULL, *savb = NULL, *net_join = NULL, *devb_name = NULL;
    int net_host = 0, net_peer = 0, auto_port = 7878;
    bool no_auto_link = false, port_set = false;
    bool restart = false, no_state = false, state_resumed = false, state_smoke = false;
    uint64_t max_cycles = 0;              /* 0 = default per mode, set below */
    double max_wall = 0, snap_secs = 0;
    uint64_t trace_from = UINT64_MAX;
    uint32_t dis_lo = 0, dis_hi = 0;
    bool headless = false, turbo = false, mute = false, allow_sleep = false, stall_probe = false, telemetry = false;
    bool show_buttons = true;
    bool on_top = false;
    char keybind[3] = {'z', 'x', 'c'};  /* A,B,C keys; --keys rebinds, arrows stay fixed */
    double next_tele = 0;

    /* Buffer diagnostics so logging does not distort host-timed link collisions. */
    static char logbuf[1 << 20];
    setvbuf(stderr, logbuf, _IOFBF, sizeof logbuf);

    /* Pre-scan --device because later options may use its memory map. */
    {
        const DeviceProfile *dev = device_default();
        for (int i = 1; i < argc - 1; i++)
            if (!strcmp(argv[i], "--device")) {
                if (!strcmp(argv[i + 1], "help") || !strcmp(argv[i + 1], "list")) {
                    device_list(stderr);
                    return 0;
                }
                dev = device_find(argv[i + 1]);
                if (!dev) {
                    fprintf(stderr, "unknown device '%s'\n", argv[i + 1]);
                    device_list(stderr);
                    return 1;
                }
            }
        if (!device_check(dev, stderr)) return 1;
        e.dev = *dev;
        e.cmu.osc3_hz = dev->osc3_hz;
        e.auto_touch = true;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--max-cycles") && i + 1 < argc) max_cycles = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--dump-bmp") && i + 1 < argc) bmp = argv[++i];
        else if (!strcmp(argv[i], "--sav") && i + 1 < argc) savoverride = argv[++i];
        else if (!strcmp(argv[i], "--persist-ram")) persist_ram = true;
        else if (!strcmp(argv[i], "--restart")) restart = true;
        else if (!strcmp(argv[i], "--no-state")) no_state = true;
        else if (!strcmp(argv[i], "--state-smoke")) state_smoke = true;
        else if (!strcmp(argv[i], "--link") && i + 1 < argc) linkrom = argv[++i];
        else if (!strcmp(argv[i], "--host")) net_host = (i + 1 < argc && argv[i+1][0] != '-')
                                                        ? atoi(argv[++i]) : 7878;
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) net_join = argv[++i];
        else if (!strcmp(argv[i], "--no-auto-link")) no_auto_link = true;
        else if (!strcmp(argv[i], "--ir-port") && i + 1 < argc) {
            auto_port = atoi(argv[++i]);
            port_set = true;
        }
        else if (!strcmp(argv[i], "--peer")) net_peer = (i + 1 < argc && argv[i+1][0] != '-')
                                                        ? atoi(argv[++i]) : 7878;
        else if (!strcmp(argv[i], "--sav-b") && i + 1 < argc) savb = argv[++i];
        else if (!strcmp(argv[i], "--device-b") && i + 1 < argc) devb_name = argv[++i];
        else if (!strcmp(argv[i], "--trace")) trace_from = 0;
        else if (!strcmp(argv[i], "--trace-from") && i + 1 < argc) trace_from = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--io-log")) e.io_log = true;
        else if (!strcmp(argv[i], "--tone-log")) e.tone_log = true;
        else if (!strcmp(argv[i], "--ir-log")) e.ir_log = true;
        else if (!strcmp(argv[i], "--nfc-log")) e.nfc_log = true;
        else if (!strcmp(argv[i], "--nfc-log-rf")) e.nfc_log_rf = true;
        else if (!strcmp(argv[i], "--no-auto-touch")) e.auto_touch = false;
        else if (!strcmp(argv[i], "--nfc-inject") && i + 1 < argc) nfc_inject = argv[++i];
        else if (!strcmp(argv[i], "--syms") && i + 1 < argc) disasm_syms_load(argv[++i]);
        else if (!strcmp(argv[i], "--pc-hook") && i + 1 < argc)
            add_pc_hook(&e, (uint32_t)strtoul(argv[++i], 0, 0), "pc-hook");
        else if (!strcmp(argv[i], "--pc-trace") && i + 1 < argc)
            add_pc_hook_ex(&e, (uint32_t)strtoul(argv[++i], 0, 0), "pc-trace", 1);
        else if (!strcmp(argv[i], "--watch-flash") && i + 2 < argc) {
            e.fwatch_lo = (uint32_t)strtoul(argv[++i], 0, 0);
            e.fwatch_hi = (uint32_t)strtoul(argv[++i], 0, 0);
            e.fwatch_budget = (i + 1 < argc && argv[i + 1][0] != '-')
                            ? (int)strtol(argv[++i], 0, 0) : 4000;
        } else if (!strcmp(argv[i], "--call-trace") && i + 2 < argc) {
            /* Repeatable; the last budget wins. */
            if (e.call_nranges < CALL_RANGES) {
                e.call_lo[e.call_nranges] = (uint32_t)strtoul(argv[++i], 0, 0);
                e.call_hi[e.call_nranges] = (uint32_t)strtoul(argv[++i], 0, 0);
                e.call_nranges++;
            } else { i += 2; fprintf(stderr, "[call] ignoring extra --call-trace range\n"); }
            e.call_budget = (i + 1 < argc && argv[i + 1][0] != '-')
                          ? (int)strtol(argv[++i], 0, 0) : 20000;
        } else if (!strcmp(argv[i], "--ir-fast")) e.ir_fast = true;
        else if (!strcmp(argv[i], "--scanlog")) {
            /* PC hooks also run for core B, where check_pc() does not. */
            scanlog = true;
            add_pc_hook(&e, 0x0251FDC2, "find-free-slot");
            add_pc_hook(&e, 0x0251FE04, "find-item-kind");
            add_pc_hook(&e, 0x0251FD38, "scan-A");
            add_pc_hook(&e, 0x0251FD6E, "scan-B");
            add_pc_hook(&e, 0x0251FC76, "slot-check");
            add_pc_hook(&e, 0x0251F3CE, "kind-slot-addr");
        } else if (!strcmp(argv[i], "--debug-strap")) e.debug_strap = true;
        else if (!strcmp(argv[i], "--flight")) {
            flightf = fopen("flight.bin", "wb");
            flight_trigger = 0x0251F3CE;   /* kind-slot-addr(kind=7,...) */
            flight_armed = true;
            flight_left = 600000;
        } else if (!strcmp(argv[i], "--rtc-mult") && i + 1 < argc) e.rtc_mult = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rtc-set") && i + 1 < argc) {
            int hh = 0, mm = 0;
            sscanf(argv[++i], "%d:%d", &hh, &mm);
            /* Store 24-hour time as BCD. */
            e.ioram[0x301918 - e.dev.io_base] = (uint8_t)(((hh / 10) << 4) | (hh % 10));
            e.ioram[0x301914 - e.dev.io_base] = (uint8_t)(((mm / 10) << 4) | (mm % 10));
        } else if (!strcmp(argv[i], "--lcd-log")) e.lcd.log = true;
        else if (!strcmp(argv[i], "--osc3") && i + 1 < argc) e.cmu.osc3_hz = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) i++;   /* pre-scanned above */
        else if (!strcmp(argv[i], "--turbo")) turbo = true;
        else if (!strcmp(argv[i], "--mute")) mute = true;
        else if (!strcmp(argv[i], "--allow-sleep")) allow_sleep = true;
        else if (!strcmp(argv[i], "--stay-awake")) e.stay_awake = true;
        else if (!strcmp(argv[i], "--no-buttons")) show_buttons = false;
        else if (!strcmp(argv[i], "--on-top")) on_top = true;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            if (!panel_keys_parse(argv[++i], keybind))
                fprintf(stderr, "[keys] bad --keys %s (three of a-z/1-9, no repeats, s/n reserved) - keeping %c%c%c\n",
                        argv[i], keybind[0], keybind[1], keybind[2]);
        } else if (!strcmp(argv[i], "--snap") && i + 1 < argc) snap_secs = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--stall-probe")) stall_probe = true;
        else if (!strcmp(argv[i], "--telemetry")) telemetry = true;
        else if (!strcmp(argv[i], "--max-wall-secs") && i + 1 < argc) max_wall = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) parse_presses(argv[++i]);
        else if (!strcmp(argv[i], "--press-b") && i + 1 < argc) parse_presses_b(argv[++i]);
        else if (!strcmp(argv[i], "--nfc-probe") && i + 1 < argc) {
            e.nfc_probe_at = strtoull(argv[++i], 0, 0);
            if (!e.nfc_probe_at) e.nfc_probe_at = 1;   /* 0 = from boot, not off */
        } else if (!strcmp(argv[i], "--touch-type") && i + 1 < argc)
            e.touch_type = (int)strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--btn-log")) e.btn_log = true;
        else if (!strcmp(argv[i], "--watch-ram") && i + 2 < argc) {
            if (e.rwatch_n < 4) {
                e.rwatch_lo[e.rwatch_n] = (uint32_t)strtoul(argv[++i], 0, 16);
                e.rwatch_hi[e.rwatch_n] = (uint32_t)strtoul(argv[++i], 0, 16);
                e.rwatch_n++;
            } else { i += 2; fprintf(stderr, "[ram] ignoring extra --watch-ram range\n"); }
            e.rwatch_budget = 4000;
        } else if (!strcmp(argv[i], "--watch-io") && i + 2 < argc) {
            e.watch_lo = (uint32_t)strtoul(argv[++i], 0, 16);
            e.watch_hi = (uint32_t)strtoul(argv[++i], 0, 16);
        } else if (!strcmp(argv[i], "--disasm") && i + 2 < argc) {
            dis_lo = (uint32_t)strtoul(argv[++i], 0, 16);
            dis_hi = (uint32_t)strtoul(argv[++i], 0, 16);
        } else if (argv[i][0] != '-') rompath = argv[i];
        else usage();
    }
    if (!rompath) usage();
    if (state_smoke && (linkrom || net_host || net_join || net_peer)) {
        fprintf(stderr, "[state] --state-smoke only supports standalone sessions\n");
        return 1;
    }
    {
        extern int t16_extra_shift[6];
        const char *ts = getenv("TAMAEMU_TSHIFT");
        if (ts) sscanf(ts, "%d %d %d %d %d %d", &t16_extra_shift[0], &t16_extra_shift[1],
                       &t16_extra_shift[2], &t16_extra_shift[3], &t16_extra_shift[4],
                       &t16_extra_shift[5]);
    }
    if (!max_cycles) {
#ifdef USE_SDL
        /* Interactive runs until closed; headless runs have a finite default. */
        max_cycles = headless ? 400000000ULL : UINT64_MAX;
#else
        max_cycles = 400000000ULL;        /* ~20 s of 20 MHz silicon */
#endif
    }
    if (state_smoke) {
        /* Test the normal shutdown path without opening an SDL window. */
        headless = true;
        if (max_cycles == UINT64_MAX) max_cycles = 4096;
    }

    fprintf(stderr, "[emu] tamaemu (%s) build %s %s (deadline pacing)\n",
            e.dev.title, __DATE__, __TIME__);
    e.rom = calloc(1, e.dev.rom_size);
    FILE *f = fopen(rompath, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", rompath); return 1; }
    size_t n = fread(e.rom, 1, e.dev.rom_size, f);
    fclose(f);
    fprintf(stderr, "[rom] %s: %zu bytes at 0x%08x\n", rompath, n, e.dev.rom_base);

    /* Unless --sav was given, put the save in the device's folder beside the ROM. */
    char savpath_buf[1024];
    const char *savpath;
    if (savoverride) {
        savpath = savoverride;
    } else {
        if (!savepath_default(savpath_buf, sizeof savpath_buf, rompath, &e.dev)) {
            fprintf(stderr, "[flash] save path is too long for %s\n", rompath);
            return 1;
        }
        if (!savepath_mkdirs(rompath, &e.dev))
            fprintf(stderr, "[flash] cannot create save folder for %s\n", rompath);
        savpath = savpath_buf;
        {
            char legacy_savpath[1024];
            if (!savepath_legacy_default(legacy_savpath, sizeof legacy_savpath,
                                         rompath, &e.dev) ||
                !savepath_migrate_legacy(legacy_savpath, savpath)) {
                fprintf(stderr, "[flash] cannot move the old save bundle into %s\n", savpath);
                return 1;
            }
        }
    }
    {
        uintptr_t sav_lock = 0;
        char lock_why[1200];
        if (!state_sav_lock_acquire(savpath, &sav_lock, lock_why, sizeof lock_why)) {
            fprintf(stderr, "[sav] refusing: %s\n", lock_why);
#ifdef USE_SDL
            if (!headless)
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "tamaemu", lock_why, NULL);
#endif
            return 1;
        }
        /* Deliberately retain sav_lock until process exit. FILE_FLAG_DELETE_ON_CLOSE
         * removes the Windows sidecar even if this process crashes. */
    }
    FILE *sf = fopen(savpath, "rb");
    if (sf) {
        size_t sn = fread(e.rom, 1, e.dev.rom_size, sf);
        fclose(sf);
        fprintf(stderr, "[flash] loaded save image %s (%zu bytes)\n", savpath, sn);
    }

    /* --persist-ram keeps battery-backed A0RAM in <sav>.ram. Reset rebuilds IVRAM, DSTRAM, and CPU state. */
    char rampath[1088];
    if (persist_ram) {
        snprintf(rampath, sizeof rampath, "%s.ram", savpath);
        FILE *rf = fopen(rampath, "rb");
        if (rf) {
            size_t rn = fread(e.a0ram, 1, e.dev.a0ram_size, rf);
            fclose(rf);
            fprintf(stderr, "[ram] restored %zu bytes from %s\n", rn, rampath);
        } else {
            fprintf(stderr, "[ram] no %s yet - starting cold\n", rampath);
        }
    }

    if (dis_hi) {
        char d[96];
        for (uint32_t p = dis_lo; p < dis_hi; ) {
            int len = disasm_one(&e, p, d, sizeof d);
            printf("%08x  ", p);
            for (int k = 0; k < len; k += 2) printf("%04x ", mem_read16(&e, p + (uint32_t)k));
            for (int k = len; k < 6; k += 2) printf("     ");
            printf(" %s\n", d);
            p += (uint32_t)len;
        }
        return 0;
    }

    bool state_interactive = false;
#ifdef USE_SDL
    state_interactive = !headless;
#endif
    bool state_eligible = state_session_eligible(state_interactive || state_smoke,
                                                  linkrom != NULL,
                                                  net_host != 0, net_join != NULL,
                                                  net_peer != 0, no_state);
    char state_why[160];
    StateResult state_result = state_restore_or_reset(&e, savpath, snapshot_build_id,
                                                       state_eligible, restart,
                                                       state_why, sizeof state_why);
    state_resumed = state_result == STATE_LOADED;
    if (state_resumed) {
        fprintf(stderr, "[state] resumed at cycle %llu pc=%08x\n",
                (unsigned long long)e.cycles, e.pc);
    } else {
        if (state_result == STATE_REJECTED || state_result == STATE_IO_ERROR)
            fprintf(stderr, "[state] not resumed: %s\n", state_why);
        fprintf(stderr, "[cpu] reset vector -> %08x\n", e.pc);
    }

    /* The smoke test changes RAM and flash, then uses the usual shutdown path. */
    if (state_smoke) {
        size_t probe = e.dev.a0ram_size ? (size_t)e.dev.a0ram_size - 1 : 0;
        /* max_cycles is normally absolute. Give a resumed smoke run a new
         * slice so it executes after restoring state. */
        if (max_cycles <= e.cycles) max_cycles = e.cycles + 4096;
        uint8_t before = e.a0ram[probe];
        e.a0ram[probe] = (uint8_t)(before + 1);
        e.rom[e.dev.rom_size - 1] ^= 0x5Au;
        e.flash_dirty = true;
        fprintf(stderr, "[state] smoke %s entry pc=%08x cycle=%llu ram=%u next=%u\n",
                state_resumed ? "resumed" : "cold", e.pc,
                (unsigned long long)e.cycles, before, e.a0ram[probe]);
    }

    /* --nfc-inject and --link cannot both answer the same ATR_REQ. */
    if (nfc_inject) {
        if (linkrom) {
            fprintf(stderr, "[phone] refusing: --nfc-inject and --link both put a "
                            "peer in the field; use one or the other.\n");
            return 1;
        }
        if (!e.dev.nfc_pn512) {
            fprintf(stderr, "[phone] refusing: %s has no NFC reader.\n", e.dev.title);
            return 1;
        }
        if (!nfcpeer_open(&e, nfc_inject)) return 1;
    }

    /* --btn-log maps each profile's debounce and action decision points. */
    if (e.btn_log) {
        static const struct { const char *dev; uint32_t cnt, gate, dbc[3], act[3]; } bl[] = {
            { "4u", 0x1420u, 0x8425Cu,
              { 0x02500152u, 0x02500242u, 0x02500352u },
              { 0x02500174u, 0x02500264u, 0x02500398u } },
            { "ps", 0x1450u, 0x8440Cu, { 0, 0x02509CC8u, 0 }, { 0, 0x02509CEAu, 0 } },
            /* Plain 4U addresses are independent of the 4U+ map. */
            { "4u-plain", 0x1514u, 0x84270u,
              { 0x024D011Eu, 0x024D020Eu, 0x024D031Eu },
              { 0x024D0140u, 0x024D0230u, 0x024D0364u } },
        };
        for (size_t k = 0; k < sizeof bl / sizeof bl[0]; k++) {
            if (strcmp(e.dev.name, bl[k].dev)) continue;
            e.btn_cnt_addr = bl[k].cnt; e.btn_gate_addr = bl[k].gate;
            for (int b = 0; b < 3; b++) {
                if (bl[k].dbc[b]) add_pc_hook_ex(&e, bl[k].dbc[b], "btn-debounce", 0);
                if (bl[k].act[b]) add_pc_hook_ex(&e, bl[k].act[b], "btn-ACTION", 0);
            }
            /* Modal input gate and pending-input flush routine. */
            if (!strcmp(bl[k].dev, "4u-plain"))
                add_pc_hook_ex(&e, 0x024CF8FAu, "input-FLUSH", 0);
            if (!strcmp(bl[k].dev, "4u")) {
                add_pc_hook_ex(&e, 0x02402CA8u, "gate-SET", 0);
                add_pc_hook_ex(&e, 0x024031BCu, "gate-CLEARED", 0);
                add_pc_hook_ex(&e, 0x024FF904u, "input-FLUSH", 1);
            }
            break;
        }
        fprintf(stderr, "[btn] logging on for %s: counters %04x gate %06x\n",
                e.dev.name, e.btn_cnt_addr, e.btn_gate_addr);
    }

    /* Auto-link follows the firmware's connect-mode state. */
    if (!linkrom && !net_host && !net_join && !net_peer) {
        e.auto_link = !no_auto_link;
        e.auto_link_port = auto_link_default_port(&e.dev, auto_port, port_set);
    }

    /* --host/--join runs one core over the socket transport. */
    if (net_host || net_join || net_peer) {
        static Link netlink;
        link_reset(&netlink);
        int ok = net_peer ? link_peer(&netlink, net_peer)
               : net_host ? link_host(&netlink, net_host)
                          : link_join(&netlink, net_join);
        if (!ok) { fprintf(stderr, "[link] no peer - continuing standalone.\n"); }
        else { e.link = &netlink; e.core_id = 0; }
    }

    if (linkrom) {
        static Emu eb; static Link link;
        memset(&eb, 0, sizeof eb);
        link_reset(&link);
        const DeviceProfile *devb = &e.dev;
        if (devb_name) {
            devb = device_find(devb_name);
            if (!devb) {
                fprintf(stderr, "unknown --device-b '%s'\n", devb_name);
                device_list(stderr);
                return 1;
            }
            if (!device_check(devb, stderr)) return 1;
        }
        char savb_buf[1024];
        if (!savb) {
            if (!savepath_default(savb_buf, sizeof savb_buf, linkrom, devb)) {
                fprintf(stderr, "[flash] save path is too long for %s\n", linkrom);
                return 1;
            }
            if (!savepath_mkdirs(linkrom, devb))
                fprintf(stderr, "[flash] cannot create save folder for %s\n", linkrom);
            savb = savb_buf;
        }
        if (savpath && savb && !strcmp(savpath, savb)) {
            fprintf(stderr, "[link] refusing: core A and B would share the same save file '%s' -- "
                            "pass distinct --sav and --sav-b paths.\n", savpath);
            return 1;
        }
        if (!load_core(&eb, devb, linkrom, savb, persist_ram)) return 1;
        e.link = &link;  e.core_id = 0;
        eb.link = &link; eb.core_id = 1;
        /* Core B inherits core A's diagnostics, clock, strap, and RTC settings. */
        eb.ir_log      = e.ir_log;
        eb.nfc_log     = e.nfc_log;
        eb.io_log      = e.io_log;
        eb.tone_log    = e.tone_log;
        eb.lcd.log     = e.lcd.log;
        eb.debug_strap = e.debug_strap;
        eb.rtc_mult    = e.rtc_mult;
        eb.stay_awake  = e.stay_awake;
        eb.no_sleep    = e.no_sleep;
        eb.touch_type  = e.touch_type;
        eb.watch_lo    = e.watch_lo;
        eb.watch_hi    = e.watch_hi;
        eb.cmu.osc3_hz = e.cmu.osc3_hz;
        /* Either core may take either link role, so both get the trace ranges. */
        for (int k = 0; k < e.call_nranges; k++) {
            eb.call_lo[k] = e.call_lo[k];
            eb.call_hi[k] = e.call_hi[k];
        }
        for (int k = 0; k < e.n_pc_hook; k++) {
            eb.pc_hook[k] = e.pc_hook[k];
            eb.pc_hook_name[k] = e.pc_hook_name[k];
            eb.pc_hook_ring[k] = e.pc_hook_ring[k];
        }
        eb.n_pc_hook = e.n_pc_hook;
        eb.call_nranges = e.call_nranges;
        eb.call_budget  = e.call_budget;
        eb.fwatch_lo     = e.fwatch_lo;
        eb.fwatch_hi     = e.fwatch_hi;
        eb.fwatch_budget = e.fwatch_budget;
        for (int k = 0; k < e.rwatch_n; k++) {
            eb.rwatch_lo[k] = e.rwatch_lo[k];
            eb.rwatch_hi[k] = e.rwatch_hi[k];
        }
        eb.rwatch_n      = e.rwatch_n;
        eb.rwatch_budget = e.rwatch_budget;
        if (e.rwatch_n)
            fprintf(stderr, "[ram] core-B watch: %d range(s), budget %d\n",
                    eb.rwatch_n, eb.rwatch_budget);

        /* Linked NFC cores exchange RF frames directly, without the IR relay. */
        if (e.dev.nfc_pn512) {
            e.nfc_peer  = &eb;
            eb.nfc_peer = &e;
        }

        if (headless) {
            uint64_t total = max_cycles;
            double next_snap = 0;
            int snap_n = 0;
            while (e.cycles < total && !e.stopped && !eb.stopped) {
                link_lockstep_run(&e, &eb, 4096, scripted_mask(e.cycles),
                             scripted_mask_b(eb.cycles));
                if (snap_secs > 0 && e.emu_secs >= next_snap) {
                    char sp[64];
                    snprintf(sp, sizeof sp, "snaps/link_a_%04d.bmp", snap_n);
                    snap_dump(&e, sp);
                    snprintf(sp, sizeof sp, "snaps/link_b_%04d.bmp", snap_n++);
                    snap_dump(&eb, sp);
                    next_snap = e.emu_secs + snap_secs;
                }
            }
            lcd_dump_bmp(&e,  "link_a.bmp");
            lcd_dump_bmp(&eb, "link_b.bmp");
            fprintf(stderr, "[link] done: A cyc=%llu B cyc=%llu, A->B=%llu B->A=%llu bytes\n",
                    (unsigned long long)e.cycles, (unsigned long long)eb.cycles,
                    (unsigned long long)link.bytes_ab, (unsigned long long)link.bytes_ba);
            if (e.dev.ir_gpio)
                fprintf(stderr, "[link] gpio-ir: A tx=%llu rx=%llu  B tx=%llu rx=%llu events\n",
                        (unsigned long long)e.irg_tx_events, (unsigned long long)e.irg_rx_events,
                        (unsigned long long)eb.irg_tx_events, (unsigned long long)eb.irg_rx_events);
            if (e.dev.nfc_pn512) { pn512_report(&e, stderr); pn512_report(&eb, stderr); }
            if (persist_ram) {
                core_persist(&e,  savpath, "A");
                core_persist(&eb, savb,    "B");
            } else {
                fprintf(stderr, "[link] headless link mode does not persist saves for "
                        "either core without --persist-ram - use throwaway "
                        "--sav/--sav-b files.\n");
            }
            return 0;
        }
#ifdef USE_SDL
        if (!headless) {
            timeBeginPeriod(1);
            SDL_Init(SDL_INIT_VIDEO);
            static char linktitle[96];
            snprintf(linktitle, sizeof linktitle, "%s -- Link", e.dev.title);
            SDL_Window *linkwin = SDL_CreateWindow(linktitle,
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                PANEL_W * PANEL_SCALE * 2 + 12, PANEL_H * PANEL_SCALE, 0);
            if (on_top) SDL_SetWindowAlwaysOnTop(linkwin, SDL_TRUE);
            SDL_Renderer *linkren = SDL_CreateRenderer(linkwin, -1, SDL_RENDERER_ACCELERATED);
            SDL_Texture *linktexa = SDL_CreateTexture(linkren, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
            SDL_Texture *linktexb = SDL_CreateTexture(linkren, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
            int focus = 0; uint8_t mask_a = 0, mask_b = 0;
            bool quit2 = false;
            uint64_t next_frame2 = 0;
            while (!quit2 && !e.stopped && !eb.stopped) {
                link_lockstep_run(&e, &eb, 4096, mask_a, mask_b);
                if (e.cycles >= next_frame2) {
                    uint64_t fcyc = (uint64_t)(e.cmu.mclk_hz > 0 ? e.cmu.mclk_hz : 2e7) / 60;
                    next_frame2 = e.cycles + (fcyc ? fcyc : 333333);
                    static uint32_t pxa[PANEL_W * PANEL_H], pxb[PANEL_W * PANEL_H];
                    int w, h;
                    lcd_render(&e,  pxa, &w, &h);
                    lcd_render(&eb, pxb, &w, &h);
                    SDL_UpdateTexture(linktexa, NULL, pxa, PANEL_W * 4);
                    SDL_UpdateTexture(linktexb, NULL, pxb, PANEL_W * 4);
                    SDL_RenderClear(linkren);
                    SDL_Rect ra = {0, 0, PANEL_W * PANEL_SCALE, PANEL_H * PANEL_SCALE};
                    SDL_Rect rb = {PANEL_W * PANEL_SCALE + 12, 0, PANEL_W * PANEL_SCALE, PANEL_H * PANEL_SCALE};
                    SDL_RenderCopy(linkren, linktexa, NULL, &ra);
                    SDL_RenderCopy(linkren, linktexb, NULL, &rb);
                    SDL_SetRenderDrawColor(linkren, 80, 160, 255, 255);
                    SDL_Rect hl = {focus ? PANEL_W * PANEL_SCALE + 12 : 0, PANEL_H * PANEL_SCALE - 4, PANEL_W * PANEL_SCALE, 4};
                    SDL_RenderFillRect(linkren, &hl);
                    SDL_RenderPresent(linkren);
                    SDL_Event ev;
                    while (SDL_PollEvent(&ev)) {
                        if (ev.type == SDL_QUIT) quit2 = true;
                        if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                            int down = ev.type == SDL_KEYDOWN;
                            if (ev.key.keysym.sym == SDLK_ESCAPE) { if (down) quit2 = true; continue; }
                            if (ev.key.keysym.sym == SDLK_TAB) { if (down) focus ^= 1; continue; }
                            if (ev.key.keysym.sym == SDLK_0 || ev.key.keysym.sym == SDLK_KP_0) {
                                if (down) set_speed(&e, &eb, 1, linkwin, linktitle);
                                continue;
                            }
                            int sdir = 0;
                            switch (ev.key.keysym.sym) {
                            case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: sdir = +1; break;
                            case SDLK_MINUS:  case SDLK_KP_MINUS:                sdir = -1; break;
                            default: break;
                            }
                            if (sdir) {
                                /* Linked cores must share one game-clock rate. */
                                if (down) set_speed(&e, &eb, periph_speed_step(e.rtc_mult, sdir),
                                                    linkwin, linktitle);
                                continue;
                            }
                            uint8_t bit = 0;
                            SDL_Keycode kc = ev.key.keysym.sym;
                            if      (kc == keybind[0] || kc == SDLK_LEFT)  bit = 1;
                            else if (kc == keybind[1] || kc == SDLK_DOWN)  bit = 2;
                            else if (kc == keybind[2] || kc == SDLK_RIGHT) bit = 4;
                            if (bit) {
                                /* Release on the pane that received the press. */
                                static int owner[3] = {-1, -1, -1};
                                int idx = (bit == 1) ? 0 : (bit == 2) ? 1 : 2;
                                int tgt = down ? focus : owner[idx];
                                if (down) owner[idx] = focus;
                                if (tgt == 0) { if (down) mask_a |= bit; else mask_a &= (uint8_t)~bit; }
                                else          { if (down) mask_b |= bit; else mask_b &= (uint8_t)~bit; }
                            }
                        }
                    }
                    static Uint64 nd;
                    Uint64 now = SDL_GetPerformanceCounter(), pf = SDL_GetPerformanceFrequency();
                    if (!nd || now > nd + pf / 4) nd = now;
                    nd += pf / 60;
                    if (now < nd) {
                        Uint64 r = (nd - now) * 1000 / pf;
                        if (r > 1) SDL_Delay((Uint32)(r - 1));
                        while (SDL_GetPerformanceCounter() < nd) { /* spin tail */ }
                    }
                }
            }

            /* Persist each core under the same rule as a standalone run. */
            /* Keep sent and delivered counts separate to expose receiver drops. */
            fprintf(stderr, "[link] done: A cyc=%llu B cyc=%llu, "
                    "A->B=%llu sent/%llu delivered, B->A=%llu sent/%llu delivered\n",
                    (unsigned long long)e.cycles, (unsigned long long)eb.cycles,
                    (unsigned long long)link.bytes_ab, (unsigned long long)eb.ir_rx_symbols,
                    (unsigned long long)link.bytes_ba, (unsigned long long)e.ir_rx_symbols);
            /* GPIO IR uses four FIFO bytes per envelope event. */
            if (e.dev.ir_gpio)
                fprintf(stderr, "[link] gpio-ir: A tx=%llu rx=%llu  B tx=%llu rx=%llu events\n",
                        (unsigned long long)e.irg_tx_events, (unsigned long long)e.irg_rx_events,
                        (unsigned long long)eb.irg_tx_events, (unsigned long long)eb.irg_rx_events);
            if (e.ir_log) { link_ir_pc_report(&e); link_ir_pc_report(&eb); }
            if (e.dev.nfc_pn512) { pn512_report(&e, stderr); pn512_report(&eb, stderr); }
            {
                bool sav_exists_a = false, sav_exists_b = false;
                { FILE *tf = fopen(savpath, "rb"); if (tf) { sav_exists_a = true; fclose(tf); } }
                { FILE *tf = fopen(savb,    "rb"); if (tf) { sav_exists_b = true; fclose(tf); } }
                if (e.flash_dirty || !sav_exists_a) {
                    FILE *of = fopen(savpath, "wb");
                    if (of) {
                        fwrite(e.rom, 1, e.dev.rom_size, of);
                        fclose(of);
                        fprintf(stderr, "[flash] core-A %llu programs, %llu erases, %llu recovery-delay hits -> %s %s\n",
                                (unsigned long long)e.flash_programs,
                                (unsigned long long)e.flash_erases,
                                (unsigned long long)e.flash_delay_hits,
                                e.flash_dirty ? "saved" : "created", savpath);
                    } else fprintf(stderr, "[flash] cannot write %s\n", savpath);
                }
                if (eb.flash_dirty || !sav_exists_b) {
                    FILE *of = fopen(savb, "wb");
                    if (of) {
                        fwrite(eb.rom, 1, eb.dev.rom_size, of);
                        fclose(of);
                        fprintf(stderr, "[flash] core-B %llu programs, %llu erases, %llu recovery-delay hits -> %s %s\n",
                                (unsigned long long)eb.flash_programs,
                                (unsigned long long)eb.flash_erases,
                                (unsigned long long)eb.flash_delay_hits,
                                eb.flash_dirty ? "saved" : "created", savb);
                    } else fprintf(stderr, "[flash] cannot write %s\n", savb);
                }
            }

            if (linktexa) SDL_DestroyTexture(linktexa);
            if (linktexb) SDL_DestroyTexture(linktexb);
            if (linkren) SDL_DestroyRenderer(linkren);
            if (linkwin) SDL_DestroyWindow(linkwin);
            return 0;
        }
#endif
    }

#ifdef USE_SDL
    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Texture *tex = NULL;
    SDL_AudioDeviceID adev = 0;
    if (!headless) {
        timeBeginPeriod(1);   /* 1 ms sleep granularity; default 15.6 ms makes
                               * SDL_Delay oversleep ~2x and dilates game time */
        SDL_Init(SDL_INIT_VIDEO | (mute ? 0 : SDL_INIT_AUDIO));
        if (!mute) {
            SDL_AudioSpec want = {0};
            want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 1;
            want.samples = 1024;
            adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
            if (adev) SDL_PauseAudioDevice(adev, 0);
        }
        win = SDL_CreateWindow(e.dev.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               PANEL_W * PANEL_SCALE,
                               PANEL_H * PANEL_SCALE + (show_buttons ? STRIP_H : 0), 0);
        /* On Windows, request topmost after creation; the creation flag can make
         * the later call a no-op without applying WS_EX_TOPMOST. */
        if (on_top) SDL_SetWindowAlwaysOnTop(win, SDL_TRUE);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
    }
#else
    (void)headless;
#endif

    uint64_t next_spin_check = 8000000, spin_lcd = 0, spin_io = 0;
    uint64_t next_frame = 0;
    bool quit = false;
    uint8_t live_mask = 0, mouse_mask = 0, synth_mask = 0;
    bool screen_off = false;
    double next_snap = 0;
    int snap_n = 0;

    clock_t wall0 = clock();
    while (!quit && e.cycles < max_cycles && !e.stopped) {
        if (max_wall > 0 && ((e.cycles & 0xFFFF) == 0) &&
            (double)(clock() - wall0) / CLOCKS_PER_SEC >= max_wall) {
            quit = true;   /* also skips the keep-window-open wait */
            break;
        }
        if (e.cycles >= trace_from && !e.halted) {
            char d[96];
            disasm_one(&e, e.pc, d, sizeof d);
            fprintf(e.tracef ? e.tracef : stderr, "%llu %08x  %s\n",
                    (unsigned long long)e.cycles, e.pc, d);
        }
        /* Detect relocated flash delay loops by signature, not address. */
        if (e.r[5] == 0xF423F && e.pc < 0x8000)
            e.flash_delay_hits++;

        /* Profile save bursts until three million quiet cycles after the last write. */
        {
            static uint64_t burst_ops, burst_start, burst_last, burst_delay0, wall_start;
            static uint32_t pages[64], npage; static uint64_t samples;
            uint64_t ops = e.flash_programs + e.flash_erases;
            if (ops != burst_ops) {
                if (!burst_start) { burst_start = e.cycles; burst_delay0 = e.flash_delay_hits;
                                    wall_start = (uint64_t)clock();
                                    npage = 0; samples = 0; memset(pages, 0, sizeof pages); }
                burst_last = e.cycles; burst_ops = ops;
            }
            if (burst_start && (e.cycles & 0x3FF) == 0) {
                samples++;
                uint32_t pg = e.pc >> 12, i;
                for (i = 0; i < npage; i += 2)
                    if (pages[i] == pg) { pages[i + 1]++; break; }
                if (i >= npage && npage < 62) { pages[npage] = pg; pages[npage + 1] = 1; npage += 2; }
            }
            if (burst_start && e.cycles - burst_last > 100000000) {
                double wall_ms = 1000.0 * (double)((uint64_t)clock() - wall_start) / CLOCKS_PER_SEC;
                fprintf(stderr, "[flash] SAVE: %.0f ms emulated (%llu cyc), %.0f ms wall incl. idle tail, "
                        "%llu erase + %llu prog, %llu delay-loop steps. Where the CPU sat:",
                        (burst_last - burst_start) / 20000.0,
                        (unsigned long long)(burst_last - burst_start), wall_ms,
                        (unsigned long long)e.flash_erases,
                        (unsigned long long)e.flash_programs,
                        (unsigned long long)(e.flash_delay_hits - burst_delay0));
                for (int k = 0; k < 3; k++) {
                    uint32_t best = 0, bi = 0;
                    for (uint32_t i = 0; i < npage; i += 2)
                        if (pages[i + 1] > best) { best = pages[i + 1]; bi = i; }
                    if (!best) break;
                    fprintf(stderr, " %05x000=%u%%", pages[bi],
                            (unsigned)(100 * pages[bi + 1] / (samples ? samples : 1)));
                    pages[bi + 1] = 0;
                }
                fprintf(stderr, "\n");
                burst_start = 0;
            }
        }
        /* TAMAEMU_LCDBURST reports redraw spacing for clock calibration. */
        {
            static int probe = -1;
            static uint64_t last_w, last_cyc;
            if (probe < 0) probe = getenv("TAMAEMU_LCDBURST") != NULL;
            if (probe && e.lcd_writes != last_w) {
                static clock_t last_wall;
                static double last_esec;
                clock_t now = clock();
                if (e.cycles - last_cyc > 2000000)
                    fprintf(stderr, "[lcdburst] cyc=%llu gap=%.2f emu-s / %.2f wall-s\n",
                            (unsigned long long)e.cycles,
                            e.emu_secs - last_esec,
                            (double)(now - last_wall) / CLOCKS_PER_SEC);
                last_cyc = e.cycles;
                last_wall = now;
                last_esec = e.emu_secs;
                last_w = e.lcd_writes;
            }
        }
        cpu_step(&e);
        check_pc(&e);

        /* Treat a long T4 park as sleep; short parks occur between animations. */
        {
            static double t4_last_running, input_at;
            if (e.ioram[0x3007A6 - e.dev.io_base] & 1) t4_last_running = e.emu_secs;
            if (live_mask | mouse_mask) input_at = e.emu_secs;
            screen_off = !allow_sleep && !e.stay_awake &&
                         (e.emu_secs - t4_last_running > 30.0) &&
                         (e.emu_secs - input_at > 2.0);
        }
        (void)screen_off;

        /* Clear the profile's sleep flag for --stay-awake. Accelerated clocks do
         * the same on devices whose firmware consumes the waking button press. */
        if ((e.stay_awake || (e.rtc_mult > 1 && e.dev.wake_press_lost)) &&
            e.dev.has_sleep_flag)
            e.a0ram[e.dev.sleep_flag - e.dev.a0ram_base] = 0;

        if (e.cycles - e.last_tick >= 256) {
            periph_tick(&e, (uint32_t)(e.cycles - e.last_tick));
            e.last_tick = e.cycles;
            uint8_t m = (uint8_t)(live_mask | mouse_mask | scripted_mask(e.cycles) | synth_mask);
            static uint8_t last_m;
            if (m != last_m) {
                fprintf(stderr, "[input] mask=%02x (live=%02x mouse=%02x synth=%02x) "
                                "t=%.3fs cyc=%llu\n",
                        m, live_mask, mouse_mask, synth_mask, e.emu_secs,
                        (unsigned long long)e.cycles);
                last_m = m;
            }
            periph_buttons(&e, m);
        }

        /* --telemetry reports tick counters and screen changes once per second. */
        if (telemetry && e.emu_secs >= next_tele) {
            next_tele = e.emu_secs + 1.0;
            static uint64_t pf[6], prc, phw; static uint32_t phash;
            uint32_t hash = 2166136261u;
            for (int y = 30; y < 130; y += 3)
                for (int x = 0; x < PANEL_W; x += 3)
                    hash = (hash ^ e.lcd.gram[y][x]) * 16777619u;
            fprintf(stderr, "[tele] t=%.0fs scr=%s T4=%llu T5=%llu T0=%llu T2=%llu rtc=%llu wakes=%llu\n",
                    e.emu_secs, hash == phash ? "STATIC" : "moving",
                    (unsigned long long)(e.t16_fires[4] - pf[4]),
                    (unsigned long long)(e.t16_fires[5] - pf[5]),
                    (unsigned long long)(e.t16_fires[0] - pf[0]),
                    (unsigned long long)(e.t16_fires[2] - pf[2]),
                    (unsigned long long)(e.rtcirq_clears - prc),
                    (unsigned long long)(e.halt_wakes - phw));
            for (int t = 0; t < 6; t++) pf[t] = e.t16_fires[t];
            prc = e.rtcirq_clears; phw = e.halt_wakes; phash = hash;
        }

        /* --stall-probe samples PCs while the firmware runs without redrawing. */
        if (stall_probe) {
            static uint64_t last_change, base;
            static uint32_t pchist[64]; static uint32_t phits[64], nph, samples;
            static int reported;
            if ((e.cycles & 0x3FFFF) == 0) {
                static uint32_t last_hash;
                uint32_t hash = 2166136261u;
                for (int y = 30; y < 130; y += 3)
                    for (int x = 0; x < PANEL_W; x += 3)
                        hash = (hash ^ e.lcd.gram[y][x]) * 16777619u;
                if (hash != last_hash) { last_change = e.cycles; last_hash = hash;
                                         nph = samples = 0; reported = 0; }
                last_change = last_change ? last_change : e.cycles;
                base = base ? base : e.cycles;
            }
            if (e.cycles - last_change > 60000000 && !reported) {   /* ~3s static */
                if ((e.cycles & 0xFFF) == 0 && samples < 100000) {
                    uint32_t pg = e.pc & 0xFFFFFFF0;
                    uint32_t i; for (i = 0; i < nph; i++) if (pchist[i] == pg) { phits[i]++; break; }
                    if (i >= nph && nph < 64) { pchist[nph] = pg; phits[nph] = 1; nph++; }
                    samples++;
                }
                if (samples >= 4000) {
                    fprintf(stderr, "[stall] screen static since cyc=%llu, top PCs:\n",
                            (unsigned long long)last_change);
                    for (int k = 0; k < 6; k++) {
                        uint32_t best = 0, bi = 0;
                        for (uint32_t i = 0; i < nph; i++) if (phits[i] > best) { best = phits[i]; bi = i; }
                        if (!best) break;
                        char d[80]; disasm_one(&e, pchist[bi], d, sizeof d);
                        fprintf(stderr, "[stall]   %08x %4u%%  %s\n", pchist[bi],
                                (unsigned)(100 * phits[bi] / samples), d);
                        phits[bi] = 0;
                    }
                    reported = 1;
                }
            }
        }

        if (snap_secs > 0 && e.emu_secs >= next_snap) {
            char sp[64];
            snprintf(sp, sizeof sp, "snaps/snap_%04d.bmp", snap_n++);
            snap_dump(&e, sp);
            fprintf(stderr, "[snap] %s cyc=%llu emu=%.1fs\n", sp,
                    (unsigned long long)e.cycles, e.emu_secs);
            next_snap = e.emu_secs + snap_secs;
        }

        if (e.cycles >= next_spin_check) {
            next_spin_check += 8000000;
            if (e.lcd_writes == spin_lcd && e.io_writes == spin_io && !e.halted)
                spin_report(&e);
            spin_lcd = e.lcd_writes; spin_io = e.io_writes;
        }

        if (e.cycles >= next_frame) {
            uint64_t fcyc = (uint64_t)(e.cmu.mclk_hz > 0 ? e.cmu.mclk_hz : 2e7) / 60;
            next_frame = e.cycles + (fcyc ? fcyc : 333333);

            /* Poll short-lived swap requests in both SDL and headless modes. */
            if (savpath) {
                static int swap_tick;
                if (++swap_tick >= 15) {          /* ~4 times a second at 60 fps */
                    swap_tick = 0;
                    char sp[1100];
                    snprintf(sp, sizeof sp, "%s.swap", savpath);
                    swap_apply(&e, sp);
                }
            }
#ifdef USE_SDL
            if (!headless) {
                static uint32_t px[PANEL_W * PANEL_H];
                int w, h; lcd_render(&e, px, &w, &h);
                if (screen_off)
                    for (int i = 0; i < PANEL_W * PANEL_H; i++) px[i] = 0xFF000000u;
                SDL_UpdateTexture(tex, NULL, px, PANEL_W * 4);
                SDL_Rect lcd = { 0, 0, PANEL_W * PANEL_SCALE, PANEL_H * PANEL_SCALE };
                /* panel_draw leaves its color set, so reset it before clearing. */
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                SDL_RenderClear(ren);
                /* An explicit destination keeps the LCD out of the button strip. */
                SDL_RenderCopy(ren, tex, NULL, &lcd);
                if (show_buttons) panel_draw(ren, (uint8_t)(live_mask | mouse_mask));
                SDL_RenderPresent(ren);

                /* Host time keeps the heartbeat running through emulation stalls. */
                if (e.btn_log) {
                    static Uint32 hb_last;
                    Uint32 now_ms = SDL_GetTicks();
                    if (now_ms - hb_last >= 500) {
                        hb_last = now_ms;
                        fprintf(stderr, "[btn] TICK wall=%ums t=%.3fs lcdw=%llu frames=%llu\n",
                                (unsigned)now_ms, e.emu_secs,
                                (unsigned long long)e.lcd_writes,
                                (unsigned long long)e.cycles);
                    }
                }

                /* Render cycle-stamped T0 piezo events into audio samples. */
                if (adev) {
                    static uint64_t acyc;
                    static unsigned ev_r;
                    static double phase, frac;
                    static uint8_t cur_on; static double cur_freq;
                    static int16_t buf[9600];  /* 200 ms cap per frame */
                    double mclk = e.cmu.mclk_hz > 0 ? e.cmu.mclk_hz : e.dev.osc3_hz;
                    if (!acyc) acyc = e.cycles;
                    /* resync after stalls/turbo bursts or ring overflow */
                    if ((double)(e.cycles - acyc) > mclk / 2 ||
                        e.tone_ev_w - ev_r > TONE_EV_N ||
                        SDL_GetQueuedAudioSize(adev) > 48000 / 5 * 2) {
                        acyc = e.cycles;
                        while (ev_r < e.tone_ev_w) {
                            struct ToneEv *ev = &e.tone_ev[ev_r % TONE_EV_N];
                            cur_on = ev->on; cur_freq = ev->freq; ev_r++;
                        }
                    }
                    int n = 0;
                    while (acyc < e.cycles && n < (int)(sizeof buf / 2) - 1) {
                        uint64_t next = e.cycles;
                        if (ev_r < e.tone_ev_w) {
                            struct ToneEv *ev = &e.tone_ev[ev_r % TONE_EV_N];
                            if (ev->cyc <= acyc) { cur_on = ev->on; cur_freq = ev->freq; ev_r++; continue; }
                            if (ev->cyc < next) next = ev->cyc;
                        }
                        double smp = (double)(next - acyc) * 48000.0 / mclk + frac;
                        int k = (int)smp; frac = smp - k;
                        if (k > (int)(sizeof buf / 2) - 1 - n) { k = (int)(sizeof buf / 2) - 1 - n; frac = 0; }
                        for (int i = 0; i < k; i++) {
                            int16_t s = 0;
                            if (cur_on && cur_freq >= 20 && cur_freq <= 20000) {
                                phase += cur_freq / 48000.0;
                                if (phase >= 1.0) phase -= 1.0;
                                s = phase < 0.5 ? 4500 : -4500;
                            }
                            buf[n++] = s;
                        }
                        acyc = next;
                    }
                    if (n > 0) SDL_QueueAudio(adev, buf, (Uint32)n * 2);
                }
                SDL_Event ev;
                /* Defer button-up one frame so a tap observed in one poll still
                 * reaches the emulated input. */
                static uint8_t mouse_down, mouse_release, key_release;
                if (show_buttons && mouse_release) { mouse_mask &= (uint8_t)~mouse_release; mouse_release = 0; }
                if (key_release) { live_mask &= (uint8_t)~key_release; key_release = 0; }
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT) quit = true;
                    /* --btn-log records host and emulated time for each event. */
                    if (e.btn_log &&
                        (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP ||
                         ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP))
                        fprintf(stderr, "[btn] SDL %-9s %s wall=%ums t=%.3fs\n",
                                ev.type == SDL_KEYDOWN   ? "KEYDOWN"   :
                                ev.type == SDL_KEYUP     ? "KEYUP"     :
                                ev.type == SDL_MOUSEBUTTONDOWN ? "MOUSEDOWN" : "MOUSEUP",
                                (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
                                    ? SDL_GetKeyName(ev.key.keysym.sym) : "",
                                (unsigned)SDL_GetTicks(), e.emu_secs);
                    if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                        int down = (ev.type == SDL_KEYDOWN);
                        uint8_t bit = 0;
                        SDL_Keycode kc = ev.key.keysym.sym;
                        if      (kc == keybind[0] || kc == SDLK_LEFT)  bit = 1;
                        else if (kc == keybind[1] || kc == SDLK_DOWN)  bit = 2;
                        else if (kc == keybind[2] || kc == SDLK_RIGHT) bit = 4;
                        switch (kc) {
                        case SDLK_ESCAPE: if (down) quit = true; break;
                        case SDLK_s:
                            if (down) { e.stay_awake = !e.stay_awake;
                                fprintf(stderr, "[stay-awake] %s\n", e.stay_awake ? "ON" : "OFF"); }
                            break;
                        case SDLK_n:            /* touch or remove an NFC point */
                            if (down && !ev.key.repeat)
                                pn512_probe_set(&e, !e.nfc_probe_at);
                            break;
                        case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
                            /* '=' as well as '+': on most layouts + is Shift+=,
                             * and requiring the shift makes the key feel broken */
                            if (down) set_speed(&e, NULL, periph_speed_step(e.rtc_mult, +1),
                                                win, e.dev.title);
                            break;
                        case SDLK_MINUS: case SDLK_KP_MINUS:
                            if (down) set_speed(&e, NULL, periph_speed_step(e.rtc_mult, -1),
                                                win, e.dev.title);
                            break;
                        case SDLK_0: case SDLK_KP_0:                    /* back to real time */
                            if (down) set_speed(&e, NULL, 1, win, e.dev.title);
                            break;
                        default: break;
                        }
                        if (bit) {
                            /* Defer keyup so short taps reach firmware debounce. */
                            if (down) { live_mask |= bit; key_release &= (uint8_t)~bit; }
                            else if (live_mask & bit) key_release |= bit;
                            else live_mask &= (uint8_t)~bit;
                        }
                    }
                    if (show_buttons) {
                        /* Track the pressed button so release clears the right bit. */
                        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                            mouse_down = (uint8_t)panel_hit(ev.button.x, ev.button.y);
                            mouse_mask = mouse_down;
                            mouse_release = 0;   /* a new press supersedes a pending deferred one */
                        }
                        if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                            if (mouse_mask & mouse_down) mouse_release = mouse_down;
                            else mouse_mask &= (uint8_t)~mouse_down;
                            mouse_down = 0;
                        }
                        if (ev.type == SDL_MOUSEMOTION && mouse_down)
                            mouse_mask = (panel_hit(ev.motion.x, ev.motion.y) == mouse_down)
                                         ? mouse_down : 0;
                        /* SDL synthesises KEYUPs on focus loss so live_mask unwinds
                         * itself; it does not do the same for mouse buttons, so
                         * alt-tabbing mid-click would leave one stuck down */
                        if (ev.type == SDL_WINDOWEVENT &&
                            ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                            mouse_mask = 0; mouse_down = 0; mouse_release = 0;
                        }
                    }
                }
                if (!turbo) {
                    /* absolute-deadline pacing: sleep toward the next 1/60 s wall
                     * deadline; oversleep self-corrects instead of accumulating */
                    static Uint64 next_deadline;
                    Uint64 now = SDL_GetPerformanceCounter();
                    Uint64 pf = SDL_GetPerformanceFrequency();
                    if (!next_deadline || now > next_deadline + pf / 4)
                        next_deadline = now;      /* (re)sync if idle or far behind */
                    next_deadline += pf / 60;
                    if (now < next_deadline) {
                        Uint64 remain_ms = (next_deadline - now) * 1000 / pf;
                        if (remain_ms > 1) SDL_Delay((Uint32)(remain_ms - 1));
                        while (SDL_GetPerformanceCounter() < next_deadline) { /* spin tail */ }
                    }
                }
            }
#else
            (void)turbo;
#endif
        }
    }

    if (e.ir_log) link_ir_pc_report(&e);

    if (state_smoke) {
        size_t probe = e.dev.a0ram_size ? (size_t)e.dev.a0ram_size - 1 : 0;
        fprintf(stderr, "[state] smoke exit pc=%08x cycle=%llu ram=%u\n",
                e.pc, (unsigned long long)e.cycles, e.a0ram[probe]);
    }

    /* Write a .sav after a flash change, or create one for a new session. */
    bool sav_exists = false;
    bool flash_ready;
    { FILE *tf = fopen(savpath, "rb"); if (tf) { sav_exists = true; fclose(tf); } }
    flash_ready = sav_exists && !e.flash_dirty;
    if (e.flash_dirty || !sav_exists) {
        if (flash_save_atomic(savpath, e.rom, e.dev.rom_size)) {
            flash_ready = true;
            fprintf(stderr, "[flash] %llu programs, %llu erases, %llu recovery-delay hits -> %s %s\n",
                    (unsigned long long)e.flash_programs,
                    (unsigned long long)e.flash_erases,
                    (unsigned long long)e.flash_delay_hits,
                    e.flash_dirty ? "saved" : "created", savpath);
        } else fprintf(stderr, "[flash] cannot write %s\n", savpath);
    }

    /* State follows the raw flash save. */
    if (state_eligible && !e.stopped && flash_ready) {
        if (state_save_on_exit(&e, savpath, snapshot_build_id, true,
                               state_why, sizeof state_why))
            fprintf(stderr, "[state] machine snapshot -> %s.state\n", savpath);
        else
            fprintf(stderr, "[state] cannot save machine snapshot: %s\n", state_why);
    }

    /* A0RAM changes every frame, so persistent RAM is always written. */
    if (persist_ram) {
        FILE *rf = fopen(rampath, "wb");
        if (rf) {
            fwrite(e.a0ram, 1, e.dev.a0ram_size, rf);
            fclose(rf);
            fprintf(stderr, "[ram] saved %u bytes -> %s\n",
                    (unsigned)e.dev.a0ram_size, rampath);
        } else fprintf(stderr, "[ram] cannot write %s\n", rampath);
    }
    fprintf(stderr, "[end] piezo tone events (T0): %u\n", e.tone_ev_w);
    fprintf(stderr, "[end] cycles=%llu pc=%08x stopped=%d halted=%d wall=%.1fs\n",
            (unsigned long long)e.cycles, e.pc, e.stopped, e.halted,
            (double)(clock() - wall0) / CLOCKS_PER_SEC);
    if (e.stopped) fprintf(stderr, "[end] stop reason: %s\n", e.stop_reason);
    for (size_t i = 0; i < NCHECK; i++)
        fprintf(stderr, "[end] checkpoint %-34s : %s\n", checkpoints[i].label,
                checkpoints[i].hit ? "HIT" : "not reached");
    {
        double esec = (double)e.cycles / (e.cmu.osc3_hz > 0 ? e.cmu.osc3_hz : 2e7);
        fprintf(stderr, "[end] ~%.1f emu-seconds: rtc secs counted=%llu, rtcirq clears=%llu (%.1f/s)",
                esec, (unsigned long long)e.rtc_seconds_counted,
                (unsigned long long)e.rtcirq_clears, e.rtcirq_clears / esec);
        for (int t = 0; t < 6; t++)
            if (e.t16_fires[t])
                fprintf(stderr, "  T%d=%.1f/s", t, e.t16_fires[t] / esec);
        fprintf(stderr, "\n[end] RTC regs: %02x:%02x:%02x (h:m:s BCD)\n",
                e.ioram[0x301918 - e.dev.io_base], e.ioram[0x301914 - e.dev.io_base],
                e.ioram[0x301910 - e.dev.io_base]);
        fprintf(stderr, "[end] sound queue: write-idx=%02x play-idx=%02x (unequal = stuck mid-jingle); "
                "T2 tone: ctl=%02x cra=%04x crb=%04x ck=%02x\n",
                e.dstram[0x1A4], e.dstram[0x1A5],
                e.ioram[0x300796 - e.dev.io_base],
                e.ioram[0x300790 - e.dev.io_base] | (e.ioram[0x300791 - e.dev.io_base] << 8),
                e.ioram[0x300792 - e.dev.io_base] | (e.ioram[0x300793 - e.dev.io_base] << 8),
                e.ioram[0x3007E4 - e.dev.io_base]);
        fprintf(stderr, "[end] halt wakes=%llu (%.1f/s); irqs taken:",
                (unsigned long long)e.halt_wakes, e.halt_wakes / esec);
        for (int v = 0; v < 80; v++)
            if (e.irq_taken[v])
                fprintf(stderr, " v%d=%llu", v, (unsigned long long)e.irq_taken[v]);
        fprintf(stderr, "\n[end] ITC en&flag: 270/280=%02x 271/281=%02x 272/282=%02x 273/283=%02x 274/284=%02x 277/287=%02x\n",
                e.ioram[0x300270-e.dev.io_base] & e.ioram[0x300280-e.dev.io_base],
                e.ioram[0x300271-e.dev.io_base] & e.ioram[0x300281-e.dev.io_base],
                e.ioram[0x300272-e.dev.io_base] & e.ioram[0x300282-e.dev.io_base],
                e.ioram[0x300273-e.dev.io_base] & e.ioram[0x300283-e.dev.io_base],
                e.ioram[0x300274-e.dev.io_base] & e.ioram[0x300284-e.dev.io_base],
                e.ioram[0x300277-e.dev.io_base] & e.ioram[0x300287-e.dev.io_base]);
    }
    if (e.dev.nfc_pn512) pn512_report(&e, stderr);
    if (e.nfc_vpeer) nfcpeer_report(&e, stderr);
    lcd_report(&e, stderr);
    if (bmp) {
        if (lcd_dump_bmp(&e, bmp) == 0) fprintf(stderr, "[end] frame dumped to %s\n", bmp);
        else fprintf(stderr, "[end] BMP dump failed\n");
    }

#ifdef USE_SDL
    if (!headless && !quit) {
        static uint32_t px[PANEL_W * PANEL_H];
        int w, h; lcd_render(&e, px, &w, &h);
        SDL_UpdateTexture(tex, NULL, px, PANEL_W * 4);
        SDL_Rect lcd = { 0, 0, PANEL_W * PANEL_SCALE, PANEL_H * PANEL_SCALE };
        /* panel_draw leaves its color set, so reset it before clearing. */
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, &lcd);
        if (show_buttons) panel_draw(ren, 0);
        SDL_RenderPresent(ren);
        SDL_Event ev;
        while (SDL_WaitEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) break;
        }
    }
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
#endif
    return e.stopped ? 2 : 0;
}
