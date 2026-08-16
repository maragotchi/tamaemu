#include "emu.h"
#include <string.h>

#define DEV const DeviceProfile *dev = &e->dev

/* Log each unknown address once, up to TOUCH_MAX entries. */
#define TOUCH_MAX 256
static uint32_t touched[TOUCH_MAX];
static int touched_n;

static bool touch_seen(uint32_t a)
{
    for (int i = 0; i < touched_n; i++)
        if (touched[i] == a) return true;
    if (touched_n < TOUCH_MAX) touched[touched_n++] = a;
    return false;
}

void mem_io_log_touch(Emu *e, uint32_t a, int write, uint32_t v, int size)
{
    if (!e->io_log) return;
    if (touch_seen(a & ~3u)) return;
    fprintf(stderr, "[io] first %s %08x size %d val %08x pc=%08x cyc=%llu\n",
            write ? "WRITE" : "READ", a, size, v, e->pc,
            (unsigned long long)e->cycles);
}

static uint8_t unmapped_read(Emu *e, uint32_t a)
{
    e->last_io_read = a;
    mem_io_log_touch(e, a, 0, 0, 1);
    return 0;
}

/* AMD-style NOR operations complete immediately, but status polls still see
 * DQ6/DQ7 busy states. Unlock commands accept byte- or word-wired offsets. */
/* Two or more reads are needed for a visible DQ6 toggle. */
#define FLASH_BUSY_READS 3

static void flash_erase_sector(Emu *e, uint32_t off)
{
    DEV;
    /* The boot-end 64 KiB uses 8 KiB sectors; other sectors are 64 KiB. */
    uint32_t base, size;
    if (dev->flash_top_boot) {
        if (off >= dev->rom_size - 0x10000)
                             { base = off & ~0x1FFFu; size = 0x2000; }
        else                 { base = off & ~0xFFFFu; size = 0x10000; }
    } else if (off < 0x10000)  { base = off & ~0x1FFFu; size = 0x2000; }
    else                     { base = off & ~0xFFFFu; size = 0x10000; }
    memset(e->rom + base, 0xFF, size);
    e->flash_erases++; e->flash_dirty = true;
    e->flash_busy = FLASH_BUSY_READS;
    e->flash_op_data = 0xFFFF;
    e->flash_toggle = false;
    fprintf(stderr, "[flash] sector erase %08x (+%x) pc=%08x\n", dev->rom_base + base, size, e->pc);
}

/* During an operation, DQ7 complements final bit 7 and DQ6 toggles per read. */
static uint16_t flash_status(Emu *e)
{
    e->flash_toggle = !e->flash_toggle;
    uint16_t s = 0;
    if (!((e->flash_op_data >> 7) & 1)) s |= 0x80;
    if (e->flash_toggle) s |= 0x40;
    if (--e->flash_busy <= 0) e->flash_busy = 0;
    return s;
}

static void flash_write(Emu *e, uint32_t a, uint32_t v, int size)
{
    DEV;
    uint32_t off = a - dev->rom_base;
    uint8_t d = (uint8_t)v;
    if (e->flash_state == 3) {                       /* program data cycle */
        uint16_t final = 0;
        for (int i = 0; i < size; i++) {
            uint8_t nb = e->rom[(off + (uint32_t)i) & (dev->rom_size - 1)] &= (uint8_t)(v >> (8 * i));
            final |= (uint16_t)nb << (8 * i);
        }
        if (e->flash_programs++ < 8)
            fprintf(stderr, "[flash] program %08x = %0*x pc=%08x\n", a, size * 2, v, e->pc);
        /* --watch-flash logs every write in the selected range. */
        if (e->fwatch_hi && a >= e->fwatch_lo && a <= e->fwatch_hi && e->fwatch_budget > 0) {
            e->fwatch_budget--;
            fprintf(stderr, "[fw%c] %08x = %0*x pc=%08x t=%.2fs\n",
                    e->core_id ? 'B' : 'A', a, size * 2, v, e->pc, e->emu_secs);
        }
        e->flash_dirty = true;
        e->flash_state = 0;
        e->flash_busy = FLASH_BUSY_READS;
        e->flash_op_data = size >= 2 ? final : (uint16_t)(0xFF00 | final);
        e->flash_toggle = false;
        return;
    }
    if (d == 0xF0) { e->flash_state = 0; e->flash_autoselect = false; return; }
    switch (e->flash_state) {
    case 0: if (d == 0xAA) e->flash_state = 1; break;
    case 1: e->flash_state = (d == 0x55) ? 2 : 0; break;
    case 2:
        if (d == 0xA0) e->flash_state = 3;
        else if (d == 0x80) e->flash_state = 4;
        else if (d == 0x90) { e->flash_autoselect = true; e->flash_state = 0;
                              fprintf(stderr, "[flash] autoselect enter pc=%08x\n", e->pc); }
        else e->flash_state = 0;
        break;
    case 4: e->flash_state = (d == 0xAA) ? 5 : 0; break;
    case 5: e->flash_state = (d == 0x55) ? 6 : 0; break;
    case 6:
        if (d == 0x30) flash_erase_sector(e, off);
        else if (d == 0x10) { memset(e->rom, 0xFF, dev->rom_size); e->flash_dirty = true;
                              fprintf(stderr, "[flash] CHIP ERASE pc=%08x\n", e->pc); }
        e->flash_state = 0;
        break;
    default: e->flash_state = 0; break;
    }
}

static uint8_t flash_read8(Emu *e, uint32_t a)
{
    DEV;
    if (e->flash_busy) return (uint8_t)flash_status(e);
    if (e->flash_autoselect) {
        /* Word offsets 0 and 1 return manufacturer and device IDs. */
        uint32_t w = (a - dev->rom_base) >> 1;
        uint8_t v;
        if ((w & 0xFF) == 0) v = (a & 1) ? 0x00 : 0x01;          /* mfr 0x0001 */
        else                 v = (a & 1) ? 0x22 : 0xF6;          /* dev 0x22F6 (29LV320DT) */
        fprintf(stderr, "[flash] autoselect read %08x -> %02x pc=%08x\n", a, v, e->pc);
        return v;
    }
    return e->rom[a - dev->rom_base];
}

/* For watched ROM, log each reader PC and 256-byte block once per process. */
static void rom_watch(Emu *e, uint32_t a)
{
    static uint64_t seen[1024];
    static int n;
    if (a < e->watch_lo || a >= e->watch_hi) return;
    uint64_t key = ((uint64_t)e->pc << 32) | (a & ~0xFFu);
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 1024) seen[n++] = key;
    /* Core tags distinguish linked saves. */
    fprintf(stderr, "[watch%c] ROMread %08x at pc=%08x cyc=%llu\n",
            e->core_id ? 'B' : 'A', a, e->pc, (unsigned long long)e->cycles);
}

/* --watch-ram logs changed A0RAM bytes with the writer PC. */
static void ram_watch(Emu *e, uint32_t a, uint8_t old, uint8_t v)
{
    if (old == v || e->rwatch_budget <= 0) return;
    for (int i = 0; i < e->rwatch_n; i++)
        if (a >= e->rwatch_lo[i] && a <= e->rwatch_hi[i]) {
            e->rwatch_budget--;
            fprintf(stderr, "[ram%c] %04x: %02x -> %02x pc=%08x t=%.4fs\n",
                    e->core_id ? 'B' : 'A', a, old, v, e->pc, e->emu_secs);
            return;
        }
}

uint8_t mem_read8(Emu *e, uint32_t a)
{
    DEV;
    if (a - dev->rom_base < dev->rom_size) {
        if (e->watch_hi) rom_watch(e, a);
        if (e->flash_busy || e->flash_autoselect) return flash_read8(e, a);
        return e->rom[a - dev->rom_base];
    }
    if (a - dev->a0ram_base < dev->a0ram_size)  return e->a0ram[a - dev->a0ram_base];
    if (a - dev->io_base < dev->io_size) { e->last_io_read = a; return periph_read8(e, a); }
    if (a - dev->ivram_base < dev->ivram_size)  return e->ivram[a - dev->ivram_base];
    if (a - dev->dstram_base < dev->dstram_size) return e->dstram[a - dev->dstram_base];
    if (a == dev->lcd_cmd_addr || a == dev->lcd_data_addr) { e->last_io_read = a; return lcd_read(e, a); }
    return unmapped_read(e, a);
}

void mem_write8(Emu *e, uint32_t a, uint8_t v)
{
    DEV;
    if (a - dev->a0ram_base < dev->a0ram_size)  {
        /* --ir-log tracks session state and headers but omits retry counters. */
        if (e->ir_log && e->a0ram[a - dev->a0ram_base] != v &&
            (a == 0xDB2u || a == 0xDB3u || a == 0x3DBCu || a == 0x3DBDu ||
             a == 0xDC0u || a == 0xD7Du || a == 0x4964u || a == 0xD88u ||
             (a >= 0xE30u && a <= 0xE5Fu)))
            fprintf(stderr, "[irst%c] %04x: %02x -> %02x pc=%08x t=%.2fs\n",
                    e->core_id ? 'B' : 'A',
                    a, e->a0ram[a - dev->a0ram_base], v, e->pc, e->emu_secs);
        /* Profiles without an absolute mode flag begin sessions from IR_TRIG. */
        if (dev->ir_mode_flag && a == dev->ir_mode_flag) {
            uint8_t old = e->a0ram[a - dev->a0ram_base];
            /* Arm the post-IR trace when a session ends. */
            if (e->ir_log && v == 0 && old != 0) {
                e->post_ir_trace = 400;
                e->post_ir_last = 0;
            }
            /* A new receiver cannot accept a whole byte instantly. */
            if (v != 0 && old == 0) periph_ir_session_begin(e);
        }
        if (e->btn_log && e->btn_cnt_addr &&
            a >= e->btn_cnt_addr && a < e->btn_cnt_addr + 3 &&
            e->a0ram[a - dev->a0ram_base] != v)
            fprintf(stderr, "[cnt] %c: %02x -> %02x pc=%08x t=%.3fs\n",
                    "ABC"[a - e->btn_cnt_addr], e->a0ram[a - dev->a0ram_base], v,
                    e->pc, e->emu_secs);
        if (e->rwatch_n) ram_watch(e, a, e->a0ram[a - dev->a0ram_base], v);
        e->a0ram[a - dev->a0ram_base] = v;
        return;
    }
    if (a == dev->lcd_cmd_addr)  { e->lcd_writes++; lcd_cmd(e, v);  return; }
    if (a == dev->lcd_data_addr) { e->lcd_writes++; lcd_data(e, v); return; }
    if (a - dev->io_base < dev->io_size) { e->io_writes++; periph_write8(e, a, v); return; }
    if (a - dev->ivram_base < dev->ivram_size)  { e->ivram[a - dev->ivram_base] = v; return; }
    if (a - dev->dstram_base < dev->dstram_size) {
        if (e->watch_hi && a >= e->watch_lo && a < e->watch_hi) {
            static int n;
            if (n < 400) {
                n++;
                fprintf(stderr, "[watch] RAMW %08x = %02x at pc=%08x cyc=%llu\n",
                        a, v, e->pc, (unsigned long long)e->cycles);
            }
        }
        e->dstram[a - dev->dstram_base] = v;
        return;
    }
    if (a - dev->rom_base < dev->rom_size) { flash_write(e, a, v, 1); return; }
    mem_io_log_touch(e, a, 1, v, 1);
}

uint16_t mem_read16(Emu *e, uint32_t a)
{
    DEV;
    if (a - dev->rom_base < dev->rom_size - 1) {
        if (e->watch_hi && (a > e->pc + 0x100 || a + 0x100 < e->pc)) rom_watch(e, a);
        if (e->flash_busy) return flash_status(e);
        if (e->flash_autoselect)
            return (uint16_t)(flash_read8(e, a) | (flash_read8(e, a + 1) << 8));
        const uint8_t *p = e->rom + (a - dev->rom_base);
        return (uint16_t)(p[0] | (p[1] << 8));
    }
    if (a - dev->a0ram_base < dev->a0ram_size - 1) {
        const uint8_t *p = e->a0ram + (a - dev->a0ram_base);
        return (uint16_t)(p[0] | (p[1] << 8));
    }
    return (uint16_t)(mem_read8(e, a) | (mem_read8(e, a + 1) << 8));
}

uint32_t mem_read32(Emu *e, uint32_t a)
{
    DEV;
    if (a - dev->rom_base < dev->rom_size - 3) {
        if (e->watch_hi && (a > e->pc + 0x100 || a + 0x100 < e->pc)) rom_watch(e, a);
        if (e->flash_busy || e->flash_autoselect)
            return (uint32_t)mem_read16(e, a) | ((uint32_t)mem_read16(e, a + 2) << 16);
        const uint8_t *p = e->rom + (a - dev->rom_base);
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (a - dev->a0ram_base < dev->a0ram_size - 3) {
        const uint8_t *p = e->a0ram + (a - dev->a0ram_base);
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (a - dev->io_base < dev->io_size - 3) { e->last_io_read = a; return periph_read32(e, a); }
    return (uint32_t)mem_read16(e, a) | ((uint32_t)mem_read16(e, a + 2) << 16);
}

void mem_write16(Emu *e, uint32_t a, uint16_t v)
{
    DEV;
    if (a - dev->a0ram_base < dev->a0ram_size - 1) {
        /* The exchange error code is written as a 16-bit value. */
        if (e->ir_log && a == 0xD88u) {
            uint16_t old = (uint16_t)(e->a0ram[0xD88] | (e->a0ram[0xD89] << 8));
            if (old != v)
                fprintf(stderr, "[irst%c] 0d88: %04x -> %04x pc=%08x t=%.2fs\n",
                        e->core_id ? 'B' : 'A', old, v, e->pc, e->emu_secs);
        }
        if (e->rwatch_n) {
            ram_watch(e, a,     e->a0ram[a - dev->a0ram_base],     (uint8_t)v);
            ram_watch(e, a + 1, e->a0ram[a - dev->a0ram_base + 1], (uint8_t)(v >> 8));
        }
        e->a0ram[a - dev->a0ram_base] = (uint8_t)v;
        e->a0ram[a - dev->a0ram_base + 1] = (uint8_t)(v >> 8);
        return;
    }
    if (a - dev->rom_base < dev->rom_size - 1) { flash_write(e, a, v, 2); return; }
    mem_write8(e, a, (uint8_t)v);
    mem_write8(e, a + 1, (uint8_t)(v >> 8));
}

void mem_write32(Emu *e, uint32_t a, uint32_t v)
{
    DEV;
    if (a - dev->a0ram_base < dev->a0ram_size - 3) {
        /* --ir-log tracks the exchange state and result words. */
        if (e->ir_log && (a == 0x3DA0u || a == 0x3DBCu)) {
            uint8_t *q = e->a0ram + (a - dev->a0ram_base);
            uint32_t old = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                           ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            if (old != v)
                fprintf(stderr, "[irsm%c] %s %08x -> %08x pc=%08x t=%.2fs\n",
                        e->core_id ? 'B' : 'A',
                        a == 0x3DA0u ? "state" : "result", old, v, e->pc, e->emu_secs);
        }
        uint8_t *p = e->a0ram + (a - dev->a0ram_base);
        if (e->rwatch_n)
            for (int i = 0; i < 4; i++)
                ram_watch(e, a + (uint32_t)i, p[i], (uint8_t)(v >> (8 * i)));
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
        return;
    }
    if (a - dev->io_base < dev->io_size - 3) { e->io_writes++; periph_write32(e, a, v); return; }
    if (a - dev->rom_base < dev->rom_size - 3) {
        flash_write(e, a, v & 0xFFFF, 2);
        flash_write(e, a + 2, v >> 16, 2);
        return;
    }
    mem_write16(e, a, (uint16_t)v);
    mem_write16(e, a + 2, (uint16_t)(v >> 16));
}
