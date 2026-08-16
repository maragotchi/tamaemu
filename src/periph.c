#include "emu.h"
#include <stdlib.h>

#define CMU_BASE 0x301B00u
#define IOB(a) e->ioram[(a) - e->dev.io_base]

/* Profiles locate the IR block; the byte pipe remains at 0x301700. */
#define IR_BASE   (e->dev.ir_base)
#define IR_DATA0  (IR_BASE + 0u)
#define IR_RXDATA (IR_BASE + 1u)   /* reading clears IR_STAT bit 0 */
#define IR_STAT   (IR_BASE + 2u)   /* bit 0 RX ready; bits 2-4 errors; bit 5 TX busy */
#define IR_CTL    (IR_BASE + 3u)
#define IR_MODE   (IR_BASE + 4u)
#define IR_TRIG   (IR_BASE + 5u)
#define IR_BAUD_LO (IR_BASE + 6u)  /* baud divisor low = clock/baud - 1 */
#define IR_BAUD_HI (IR_BASE + 7u)
#define IR_LAST    (IR_BASE + 7u)
#define SIO_RX   0x301700u    /* receive data */
#define SIO_TX   0x301704u    /* transmit data */
#define SIO_CTL  0x301708u    /* control (0x1F02 config; bit0 enable) */
#define SIO_STAT 0x301714u    /* status; bit 0x40 = busy */

static void ir_tx_byte(Emu *e, uint8_t byte);

static void watch_read(Emu *e, uint32_t a, int size);
static void watch_write(Emu *e, uint32_t a, uint32_t v, int size);

static uint32_t ioram_r32(Emu *e, uint32_t a)
{
    uint8_t *p = e->ioram + (a - e->dev.io_base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ioram_w32(Emu *e, uint32_t a, uint32_t v)
{
    uint8_t *p = e->ioram + (a - e->dev.io_base);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void cmu_recalc(Emu *e)
{
    Cmu *c = &e->cmu;
    uint32_t oscsel  = (c->sccr >> 2) & 3;
    uint32_t osc3div = (c->sccr >> 8) & 7;
    uint32_t mclkdiv = (c->sccr >> 12) & 1;
    uint32_t pllindiv = (c->sccr >> 20) & 0xF;
    uint32_t pllpowr = c->pllc & 1;
    uint32_t plln    = (c->pllc >> 4) & 0xF;
    double f;
    const char *src;
    if (oscsel == 1) { f = 32768.0; src = "OSC1"; }
    else if (oscsel == 3 && pllpowr) {
        f = c->osc3_hz / (double)(pllindiv + 1) * (double)(plln + 1);
        src = "PLL";
    } else {
        f = c->osc3_hz / (double)(1u << osc3div);
        src = "OSC3";
    }
    if (mclkdiv) f /= 2.0;
    if (f != c->mclk_hz) {
        static int logged;
        c->mclk_hz = f;
        if (e->core_id == 0 && (logged < 16 || e->io_log)) {
            fprintf(stderr, "[cmu] MCLK = %.4f MHz (src=%s sccr=%08x pllc=%08x, OSC3=%.4f MHz) pc=%08x\n",
                    f / 1e6, src, c->sccr, c->pllc, c->osc3_hz / 1e6, e->pc);
            if (++logged == 16)
                fprintf(stderr, "[cmu] (further MCLK toggles suppressed; use --io-log to keep them)\n");
        }
    }
}

static void cmu_write32(Emu *e, uint32_t a, uint32_t v)
{
    Cmu *c = &e->cmu;
    uint32_t off = a - CMU_BASE;
    if (off == 0x24) { c->unlocked = ((v & 0xFF) == 0x96); ioram_w32(e, a, v); return; }
    if (off <= 0x14 && !c->unlocked) {
        fprintf(stderr, "[cmu] write %08x=%08x BLOCKED (protected) pc=%08x\n", a, v, e->pc);
        return;
    }
    ioram_w32(e, a, v);
    switch (off) {
    case 0x08: c->sccr = v; cmu_recalc(e); break;
    case 0x0C: c->pllc = v; cmu_recalc(e); break;
    case 0x14: c->opt = v; break;
    default: break;
    }
}

void periph_write32(Emu *e, uint32_t a, uint32_t v)
{
    if (e->watch_hi) watch_write(e, a, v, 4);
    mem_io_log_touch(e, a, 1, v, 4);
    if (e->ir_log && a >= 0x301700u && a <= 0x30171Fu)
        fprintf(stderr, "[ir%c] wr32 %08x = %08x pc=%08x\n",
                e->core_id ? 'B' : 'A', a, v, e->pc);
    if (a == SIO_TX) {
        ioram_w32(e, a, v);
        ir_tx_byte(e, (uint8_t)v);
        return;
    }
    if (a - CMU_BASE <= 0x24) { cmu_write32(e, a, v); return; }
    if (a == 0x301900) {                       /* RTC int status: write 1 to clear */
        if (v & 1) { IOB(0x301900) &= (uint8_t)~1u; e->rtcirq_clears++; }
        return;
    }
    ioram_w32(e, a, v);
}

uint32_t periph_read32(Emu *e, uint32_t a)
{
    if (e->watch_hi) watch_read(e, a, 4);
    /* Never report serial busy; firmware waits synchronously for it to clear. */
    if (a == SIO_STAT) {
        uint32_t s = ioram_r32(e, a) & ~0x40u;
        if (e->ir_log) {
            static uint32_t seen[32]; static int n;
            int k = 0; for (; k < n; k++) if (seen[k] == e->pc) break;
            if (k == n && n < 32) { seen[n++] = e->pc;
                fprintf(stderr, "[ir%c] rd SIO_STAT -> %08x pc=%08x\n",
                        e->core_id ? 'B' : 'A', s, e->pc); }
        }
        mem_io_log_touch(e, a, 0, s, 4);
        return s;
    }
    if (a == SIO_RX) {                 /* reading consumes the received byte */
        uint32_t b = ioram_r32(e, a);
        IOB(IR_STAT) &= (uint8_t)~0x01u;
        mem_io_log_touch(e, a, 0, b, 4);
        return b;
    }
    uint32_t v = ioram_r32(e, a);
    mem_io_log_touch(e, a, 0, v, 4);
    return v;
}

static void itc_scan(Emu *e);

/* T0 is audible when running, PTM is on, its clock is enabled, and 0 < CRA < CRB. */
static void tone_update(Emu *e)
{
    static const uint32_t pd[8] = {1,2,4,16,64,256,1024,4096};
    uint16_t cra = (uint16_t)(IOB(0x300780) | (IOB(0x300781) << 8));
    uint16_t crb = (uint16_t)(IOB(0x300782) | (IOB(0x300783) << 8));
    uint8_t  ctl = IOB(0x300786), ck = IOB(0x3007E0);
    uint8_t on = (ctl & 1) && (ctl & 4) && (ck & 8) && cra > 0 && cra < crb;
    double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
    float freq = (float)(mhz / pd[ck & 7] / (double)(crb + 1));
    if (on == e->tone_on && (!on || freq == e->tone_freq)) return;
    e->tone_on = on; e->tone_freq = freq;
    struct ToneEv *ev = &e->tone_ev[e->tone_ev_w % TONE_EV_N];
    ev->cyc = e->cycles; ev->freq = freq; ev->on = on;
    e->tone_ev_w++;
    if (e->tone_log)
        fprintf(stderr, "[tone] %10.2fms => %s %.1fHz\n",
                e->cycles * 1000.0 / mhz, on ? "ON " : "off", freq);
}

/* IR registers: B10 TX, B11 RX, B12 status, B14 mode, B15 RX enable,
 * and B16/B17 baud divisor. ITC bits 3/4/5 map to vectors 60/61/62. */

/* 8N1 byte time. IR uses a fixed 576 kHz divisor clock; NFC uses MCLK/16. */
static uint64_t ir_byte_cycles(Emu *e)
{
    uint32_t div = (uint32_t)IOB(IR_BAUD_LO) | ((uint32_t)(IOB(IR_BAUD_HI) & 0xF) << 16);
    double mclk = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
    double base = e->dev.nfc_pn512 ? mclk / 16.0 : 576000.0;
    double baud = base / (double)(div + 1);
    if (!(baud >= 300.0 && baud <= 1000000.0)) baud = 115200.0;
    double c = mclk * 10.0 / baud;
    return (uint64_t)(c > 1.0 ? c : 1.0);
}

static void ir_raise(Emu *e, int bit)      /* ITC bit maps to vector 57 + bit */
{
    IOB(0x300286) |= (uint8_t)(1u << bit);
    itc_scan(e);
}

/* NFC always uses TX-done bit 5. IR uses whichever TX line is enabled. */
static void ir_tx_done_raise(Emu *e)
{
    if (e->dev.nfc_pn512) { ir_raise(e, 5); return; }
    uint8_t en = IOB(0x300276);
    if (en & 0x20)      ir_raise(e, 5);
    else if (en & 0x08) ir_raise(e, 3);
    else if (en & 0x10) ir_raise(e, 4);
}

/* B15 pauses PN512 replies; unread data holds the next reply back. */
static void nfc_pump(Emu *e)
{
    Pn512 *n = &e->nfc;
    if (n->txq_r == n->txq_w) return;
    if (!IOB(IR_TRIG)) return;
    if (IOB(IR_STAT) & 0x01u) {
        if (e->cycles < n->reply_at + 64 * ir_byte_cycles(e)) return;
        e->ir_overruns++;
    }
    IOB(IR_RXDATA) = n->txq[n->txq_r++ % PN512_TXQ_N];
    IOB(IR_STAT) |= 0x01u;
    n->reply_at = e->cycles;               /* starts the overrun window */
    ir_raise(e, 4);                        /* RX interrupt */
}

static void ir_tx_byte(Emu *e, uint8_t byte)
{
    e->ir_tx_symbols++;
    /* Log emulated and host time so separate processes can be aligned. */
    if (e->ir_log)
        fprintf(stderr, "[ir%c] TX#%llu byte=%02x ctl=%02x pc=%08x t=%.6f h=%llu\n",
                e->core_id ? 'B' : 'A', (unsigned long long)e->ir_tx_symbols,
                byte, IOB(IR_CTL), e->pc, e->emu_secs,
                (unsigned long long)link_now_us());
    if (e->dev.nfc_pn512) {
        /* PN512 replies return through nfc_pump, not the peer link. */
        pn512_host_byte(e, byte);
    } else if (e->link) {
        /* Wire duration comes from the firmware's baud divisor. */
        double mclk = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
        link_set_tx_byte_us(e->link, (unsigned)(ir_byte_cycles(e) * 1000000.0 / mclk));
        link_tx(e->link, e->core_id, byte);
    }
    /* Clear TX busy before raising TX done. */
    IOB(IR_STAT) &= (uint8_t)~0x20u;
    if (!e->ir_fast) {
        /* TX remains unready until the byte completes. */
        e->ir_tx_ready_at = e->cycles + ir_byte_cycles(e);
        return;
    }
    ir_tx_done_raise(e);
}

/* Receiver startup takes one byte-time. */
void periph_ir_session_begin(Emu *e)
{
    e->ir_rx_next_at = e->cycles + ir_byte_cycles(e);
}

/* Read RAM without I/O side effects; invalid addresses return zero. */
static uint8_t ram_peek8(Emu *e, uint32_t a)
{
    const DeviceProfile *d = &e->dev;
    if (a - d->a0ram_base  < d->a0ram_size)  return e->a0ram[a - d->a0ram_base];
    if (a - d->ivram_base  < d->ivram_size)  return e->ivram[a - d->ivram_base];
    if (a - d->dstram_base < d->dstram_size) return e->dstram[a - d->dstram_base];
    return 0;
}

/* Session state may be an absolute byte or a byte reached through a context pointer. */
uint8_t periph_ir_session_state(Emu *e)
{
    const DeviceProfile *d = &e->dev;
    if (d->ir_mode_flag)
        return ram_peek8(e, d->ir_mode_flag);
    if (!d->ir_ctx_ptr) return 0;
    uint32_t p = d->ir_ctx_ptr;
    uint32_t ctx = (uint32_t)ram_peek8(e, p)            |
                   (uint32_t)ram_peek8(e, p + 1) <<  8  |
                   (uint32_t)ram_peek8(e, p + 2) << 16  |
                   (uint32_t)ram_peek8(e, p + 3) << 24;
    /* Reject stale context pointers unless they land in mapped RAM. */
    if (!ctx) return 0;
    if (ctx - d->a0ram_base  >= d->a0ram_size &&
        ctx - d->ivram_base  >= d->ivram_size &&
        ctx - d->dstram_base >= d->dstram_size) return 0;
    return ram_peek8(e, ctx + IR_CTX_STATE);
}

/* iD and iD L must receive peer traffic before entering a session. */
static int ir_rx_gated_on_session(Emu *e)
{
    return e->dev.ir_mode_flag != 0;
}

static void ir_rx_poll(Emu *e)
{
    /* Unarmed receivers lose bytes. Some profiles also gate on session state. */
    if ((ir_rx_gated_on_session(e) && !periph_ir_session_state(e)) || !IOB(IR_TRIG)) {
        while (e->link && link_rx_pending(e->link, e->core_id)) {
            link_rx(e->link, e->core_id);
            e->ir_rx_dropped_idle++;
            /* Discarded traffic may still raise the peer-detect interrupt. */
            if (periph_ir_session_state(e) != 1) {
                IOB(0x300287) |= 0x08u;      /* flag latches even if masked */
                itc_scan(e);
            }
        }
        return;
    }
    if (!e->ir_fast && e->cycles < e->ir_rx_next_at) return;
    /* Hold unread data briefly, then allow a hardware-style overrun. */
    if (IOB(IR_STAT) & 0x01u) {
        if (e->cycles < e->ir_rx_next_at + 64 * ir_byte_cycles(e)) return;
        e->ir_overruns++;
    }
    if (!e->link || !link_rx_pending(e->link, e->core_id)) return;
    int b = link_rx(e->link, e->core_id);
    if (b < 0) return;
    e->ir_rx_symbols++;
    /* Each queued byte is another byte-time of receiver backlog. */
    if (e->ir_log) {
        int qi = (e->link && !e->link->net) ? e->core_id : 0;
        fprintf(stderr, "[ir%c] RX#%llu byte=%02x t=%.6f h=%llu q=%u\n",
                e->core_id ? 'B' : 'A', (unsigned long long)e->ir_rx_symbols, (unsigned)b,
                e->emu_secs, (unsigned long long)link_now_us(),
                e->link ? e->link->tail[qi] - e->link->head[qi] : 0u);
    }
    IOB(SIO_RX)    = (uint8_t)b;
    IOB(IR_RXDATA) = (uint8_t)b;
    /* Set RX ready only; bits 2-4 report errors. */
    IOB(IR_STAT) |= 0x01u;
    /* Pace UART delivery at one byte-time. GPIO IR has its own clock. */
    e->ir_rx_next_at = e->cycles + ir_byte_cycles(e);
    /* UART flags latch while their interrupts are masked. */
    ir_raise(e, 4);
}

void periph_write8(Emu *e, uint32_t a, uint8_t v)
{
    if (e->watch_hi) watch_write(e, a, v, 1);
    mem_io_log_touch(e, a, 1, v, 1);
    if (a - CMU_BASE <= 0x27) {   /* merge byte writes into the 32-bit CMU model */
        uint32_t word = a & ~3u;
        uint32_t cur = ioram_r32(e, word);
        int sh = (a & 3) * 8;
        cmu_write32(e, word, (cur & ~(0xFFu << sh)) | ((uint32_t)v << sh));
        return;
    }
    if (a >= 0x300280 && a <= 0x30028F) {         /* ITC cause flags: write-1-to-clear */
        e->ioram[a - e->dev.io_base] &= (uint8_t)~v;
        itc_scan(e);
        return;
    }
    if (a == 0x3002A9) {                          /* PN512 IRQ-pin flag: write-1-to-clear */
        e->ioram[a - e->dev.io_base] &= (uint8_t)~v;
        itc_scan(e);
        return;
    }
    if (a == 0x301900) {                          /* RTC int status: write 1 to clear */
        if (v & 1) { e->ioram[a - e->dev.io_base] &= (uint8_t)~1u; e->rtcirq_clears++; }
        return;
    }
    /* Battery conversions complete immediately with a healthy reading. */
    if (a == 0x300544 && (v & 0x06)) {
        e->ioram[a - e->dev.io_base] = v;                /* ISR clears the start bits */
        /* 0x2E3 or lower reports a low battery. */
        IOB(0x300540) = 0x50; IOB(0x300541) = 0x03;
        IOB(0x300548) = 0x50; IOB(0x300549) = 0x03;
        IOB(0x300287) |= 2;
        itc_scan(e);
        return;
    }
    /* GPIO-IR sends one mark per burst of TX-bit carrier edges. */
    if (e->dev.ir_gpio && a == 0x300380) {
        uint8_t old = e->irg_port_latch;
        e->irg_port_latch = v;
        e->ioram[a - e->dev.io_base] = v;
        if ((old ^ v) & (uint8_t)(1u << e->dev.ir_gpio_tx_bit)) {
            if (!e->irg_tx_burst) {
                e->irg_tx_burst = 1;
                e->irg_tx_start = e->cycles;
                e->ir_active_at = e->emu_secs;
            }
            e->irg_tx_last_edge = e->cycles;
        }
        return;
    }
    if (a >= IR_BASE && a <= IR_LAST) {
        uint8_t old_trig = IOB(IR_TRIG);
        e->ir_active_at = e->emu_secs;
        uint8_t old = e->ioram[a - e->dev.io_base];
        e->ioram[a - e->dev.io_base] = v;
        if (e->ir_log && a != IR_BAUD_LO && a != IR_BAUD_HI)
            fprintf(stderr, "[ir%c] wr %08x = %02x pc=%08x\n",
                    e->core_id ? 'B' : 'A', a, v, e->pc);
        /* B13's top bits gate the carrier. */
        if (a == IR_CTL) {
            int on_now = (v & 0xC0) != 0, on_before = (old & 0xC0) != 0;
            if (on_now != on_before) {
                double hz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
                uint64_t d = e->ir_edges ? e->cycles - e->ir_last_edge_cyc : 0;
                e->ir_edges++;
                if (e->ir_log)
                    fprintf(stderr, "[ir%c] CARRIER %s after %9.1fus (%llu cyc) pc=%08x\n",
                            e->core_id ? 'B' : 'A', on_now ? "ON " : "OFF",
                            d * 1e6 / hz, (unsigned long long)d, e->pc);
                e->ir_last_edge_cyc = e->cycles;
            }
        }
        if (a == IR_DATA0) ir_tx_byte(e, v);
        /* Arming the receiver restarts its one-byte initialization delay. */
        if (a == IR_TRIG && v && !old_trig) periph_ir_session_begin(e);
        if (e->ir_log && (a == IR_BAUD_LO || a == IR_BAUD_HI)) {
            uint32_t div = (uint32_t)IOB(IR_BAUD_LO) | ((uint32_t)(IOB(IR_BAUD_HI) & 0xF) << 16);
            fprintf(stderr, "[ir%c] baud divisor = %u (=> %.0f baud @576k, %.0f @1.152M)\n",
                    e->core_id ? 'B' : 'A', div,
                    576000.0 / (div + 1), 1152000.0 / (div + 1));
        }
        return;
    }
    if (e->tone_log && ((a >= 0x300780 && a <= 0x3007AF) ||
                        (a >= 0x3007E0 && a <= 0x3007EB) || a == 0x3007DC)) {
        static const char *rn[8] = {"CRA.l","CRA.h","CRB.l","CRB.h","TC.l","TC.h","CTL","x7"};
        double ms = e->cmu.mclk_hz > 0 ? e->cycles * 1000.0 / e->cmu.mclk_hz : 0;
        char nm[16];
        if (a == 0x3007DC) snprintf(nm, sizeof nm, "PAUSE");
        else if (a >= 0x3007E0) snprintf(nm, sizeof nm, "T%u.clkctl", (unsigned)(a - 0x3007E0) / 2);
        else snprintf(nm, sizeof nm, "T%u.%s", (unsigned)(a - 0x300780) / 8, rn[(a - 0x300780) % 8]);
        fprintf(stderr, "[tone] %10.2fms %-10s(%08x) = %02x pc=%08x\n", ms, nm, a, v, e->pc);
    }
    /* Restarting T2 emits one decaying note even if firmware leaves the timer on. */
    if (a == 0x3007E4 && (v & 8)) {
        static const uint32_t pd[8] = {1,2,4,16,64,256,1024,4096};
        double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
        uint16_t cra = (uint16_t)(IOB(0x300790) | (IOB(0x300791) << 8));
        uint16_t crb = (uint16_t)(IOB(0x300792) | (IOB(0x300793) << 8));
        if (crb > 1) {
            e->t2_freq = mhz / pd[v & 7] / (double)(crb + 1);
            e->t2_duty = (cra > 0 && cra < crb) ? (double)cra / (double)(crb + 1) : 0.5;
            e->t2_attacks++;
        }
    }
    if (a >= 0x300780 && a <= 0x3007AF) {
        int x = (int)((a - 0x300780) / 8);
        int off = (int)((a - 0x300780) % 8);
        if (off == 6) {
            if (v & 2) { e->t16[x].counter = 0; e->t16[x].accum = 0; }
            e->ioram[a - e->dev.io_base] = v & (uint8_t)~2u; /* PRSET self-clears */
            if (x == 0) tone_update(e);
            return;
        }
        if (off == 4) {
            e->t16[x].counter = (uint16_t)((e->t16[x].counter & 0xFF00) | v);
            e->t16[x].a_fired = 0;
            e->ioram[a - e->dev.io_base] = v;
            return;
        }
        if (off == 5) {
            e->t16[x].counter = (uint16_t)((e->t16[x].counter & 0x00FF) | ((uint16_t)v << 8));
            e->ioram[a - e->dev.io_base] = v;
            return;
        }
    }
    e->ioram[a - e->dev.io_base] = v;
    if ((a >= 0x300270 && a <= 0x30027F) || a == 0x3002A6) itc_scan(e);
    if ((a >= 0x300780 && a <= 0x300783) || a == 0x3007E0) tone_update(e);
}

static void watch_read(Emu *e, uint32_t a, int size)
{
    if (e->core_id != 0) return;   /* only core A logs watch hits */
    static uint64_t seen[512];
    static int n;
    if (a < e->watch_lo || a >= e->watch_hi) return;
    uint64_t key = ((uint64_t)e->pc << 32) | a;
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 512) seen[n++] = key;
    fprintf(stderr, "[watch] read%d %08x at pc=%08x cyc=%llu\n",
            size, a, e->pc, (unsigned long long)e->cycles);
}

static void watch_write(Emu *e, uint32_t a, uint32_t v, int size)
{
    if (e->core_id != 0) return;   /* only core A logs watch hits */
    static uint64_t seen[1024];
    static int n;
    if (a < e->watch_lo || a >= e->watch_hi) return;
    uint64_t key = ((uint64_t)e->pc << 40) | ((uint64_t)(a & 0xFFFFF) << 20) | (v & 0xFFFFF);
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 1024) seen[n++] = key;
    fprintf(stderr, "[watch] WRITE%d %08x = %x at pc=%08x cyc=%llu\n",
            size, a, v, e->pc, (unsigned long long)e->cycles);
}

uint8_t periph_read8(Emu *e, uint32_t a)
{
    if (e->watch_hi) watch_read(e, a, 1);
    /* P00-P02 are active-low buttons; P06 polarity is profile-specific. */
    if (a == 0x300380) {
        uint8_t p06 = (e->debug_strap == e->dev.debug_strap_high) ? 0x40 : 0x00;
        uint8_t v = (uint8_t)((0x07u | p06) & ~(e->btn_mask & 7));
        /* The GPIO IR demodulator idles high and pulls low for a carrier mark. */
        if (e->dev.ir_gpio) {
            uint8_t rb = (uint8_t)(1u << e->dev.ir_gpio_rx_bit);
            v |= rb;
            if (e->irg_rx_marking) v &= (uint8_t)~rb;
        }
        mem_io_log_touch(e, a, 0, v, 1);
        return v;
    }
    /* Reading IR RX data clears the ready flag. */
    if (a == IR_RXDATA) {
        uint8_t v = e->ioram[a - e->dev.io_base];
        IOB(IR_STAT) &= (uint8_t)~0x01u;
        if (e->ir_log)
            fprintf(stderr, "[ir%c] rd B11 = %02x (rx-ready cleared)\n",
                    e->core_id ? 'B' : 'A', v);
        mem_io_log_touch(e, a, 0, v, 1);
        return v;
    }
    if (a == IR_STAT) {
        uint8_t s = e->ioram[a - e->dev.io_base];
        /* TX ready stays low while a byte is shifting out. */
        if (e->ir_fast || e->cycles >= e->ir_tx_ready_at) s |= 0x02u;
        else                                             s &= (uint8_t)~0x02u;
        mem_io_log_touch(e, a, 0, s, 1);
        return s;
    }
    if (e->ir_log && a >= IR_BASE && a <= IR_LAST) {
        static uint64_t seen[64]; static int n;
        uint64_t key = ((uint64_t)e->pc << 16) | a;
        int k = 0; for (; k < n; k++) if (seen[k] == key) break;
        if (k == n && n < 64) { seen[n++] = key;
            fprintf(stderr, "[ir%c] rd %08x -> %02x pc=%08x\n",
                    e->core_id ? 'B' : 'A', a, e->ioram[a - e->dev.io_base], e->pc); }
    }
    uint8_t v = e->ioram[a - e->dev.io_base];
    mem_io_log_touch(e, a, 0, v, 1);
    return v;
}

/* Button edges raise ITC port-input flags 0-2. */
void periph_buttons(Emu *e, uint8_t mask)
{
    uint8_t newly = mask & (uint8_t)~e->btn_mask & 7;
    e->btn_mask = mask & 7;
    if (newly) {
        if (e->btn_log) {
            unsigned gate = 0, ca = 0, cb = 0, cc = 0;
            if (e->btn_gate_addr &&
                e->btn_gate_addr - e->dev.dstram_base < e->dev.dstram_size)
                gate = e->dstram[e->btn_gate_addr - e->dev.dstram_base];
            if (e->btn_cnt_addr &&
                e->btn_cnt_addr + 2 - e->dev.a0ram_base < e->dev.a0ram_size) {
                uint32_t o = e->btn_cnt_addr - e->dev.a0ram_base;
                ca = e->a0ram[o]; cb = e->a0ram[o + 1]; cc = e->a0ram[o + 2];
            }
            fprintf(stderr, "[btn] EDGE %s%s%s  gate=%02x cnt A/B/C=%02x/%02x/%02x"
                            "  %s  t=%.3fs cyc=%llu\n",
                    (newly & 1) ? "A" : "", (newly & 2) ? "B" : "",
                    (newly & 4) ? "C" : "", gate, ca, cb, cc,
                    !e->btn_cnt_addr ? "gates not mapped for this device"
                    : gate ? "gated: modal routine busy"
                         : ((newly & 1) && ca) || ((newly & 2) && cb) ||
                           ((newly & 4) && cc)
                           ? "gated: previous press not consumed" : "should act",
                    e->emu_secs, (unsigned long long)e->cycles);
        }
        IOB(0x300280) |= newly;
        itc_scan(e);
    }
}

/* T16 channels are eight bytes: compare A/B, counter, then control.
 * ITC uses bits 2/3 for even channels and 6/7 for odd channels.
 * Timer B/A vectors are 30/31, then increase by four per channel. */
static const uint32_t presc_div[8] = {1, 2, 4, 16, 64, 256, 1024, 4096};
int t16_extra_shift[6] = {0, 0, 0, 0, 0, 0};   /* overridable through the environment */

static void t16_set_flag(Emu *e, int x, int is_a)
{
    uint32_t freg = 0x300282 + (uint32_t)(x / 2);
    int bit = (x & 1) ? (is_a ? 7 : 6) : (is_a ? 3 : 2);
    IOB(freg) |= (uint8_t)(1u << bit);
    if (!is_a) e->t16_fires[x]++;
}

static void t16_run(Emu *e, int x, uint32_t clocks)
{
    uint32_t base = 0x300780 + (uint32_t)x * 8;
    uint16_t cra = (uint16_t)(IOB(base) | (IOB(base + 1) << 8));
    uint16_t crb = (uint16_t)(IOB(base + 2) | (IOB(base + 3) << 8));
    T16 *t = &e->t16[x];
    while (clocks--) {
        t->counter++;
        /* Presets may pass a compare value, so >= means due. Latch each edge once. */
        if (cra && !t->a_fired && t->counter >= cra) {
            t16_set_flag(e, x, 1);
            t->a_fired = 1;
        }
        if (t->counter >= crb) {
            t16_set_flag(e, x, 0);
            t->counter = 0;
            t->a_fired = 0;
        }
    }
    IOB(base + 4) = (uint8_t)t->counter;
    IOB(base + 5) = (uint8_t)(t->counter >> 8);
}

static void itc_consider(Emu *e, uint32_t vec, uint8_t lvl)
{
    e->wake_req = true;
    if (lvl > e->pending_level || !e->pending_irq) {
        e->pending_irq = vec; e->pending_level = lvl;
    }
}

static void itc_scan(Emu *e)
{
    static const uint8_t vec_b[6] = {30, 34, 38, 42, 46, 50};
    e->pending_irq = 0; e->pending_level = 0; e->wake_req = false;
    /* Port interrupts 0-3 map to vectors 16-19. */
    {
        uint8_t act = IOB(0x300270) & IOB(0x300280) & 0x0F;
        for (int p = 0; p < 4; p++) {
            if (!(act & (1u << p))) continue;
            uint8_t lr = IOB(0x300260 + (uint32_t)(p / 2));
            uint8_t lvl = (p & 1) ? (lr >> 4) & 7 : lr & 7;
            itc_consider(e, 16u + (uint32_t)p, lvl);
        }
    }
    for (int x = 0; x < 6; x++) {
        uint32_t en = 0x300272 + (uint32_t)(x / 2), fl = 0x300282 + (uint32_t)(x / 2);
        uint8_t act = IOB(en) & IOB(fl);
        int b_bit = (x & 1) ? 6 : 2, a_bit = (x & 1) ? 7 : 3;
        uint8_t lvl_reg = IOB(0x300266 + (uint32_t)(x / 2));
        uint8_t lvl = (x & 1) ? (lvl_reg >> 4) & 7 : lvl_reg & 7;
        if (act & (1u << b_bit)) itc_consider(e, vec_b[x], lvl);
        if (act & (1u << a_bit)) itc_consider(e, vec_b[x] + 1u, lvl);
    }
    /* RTC: bit 2, vector 65. */
    if (IOB(0x300277) & IOB(0x300287) & 4) {
        uint8_t lvl = (IOB(0x30026A) >> 4) & 7;
        itc_consider(e, 65, lvl ? lvl : 1);
    }
    /* A/D converter: bit 1, vector 64. */
    if (IOB(0x300277) & IOB(0x300287) & 2) {
        uint8_t lvl = IOB(0x30026A) & 7;
        itc_consider(e, 64, lvl ? lvl : 1);
    }
    /* IR port activity uses vector 68 at priority 4. */
    if (IOB(0x300277) & IOB(0x300287) & 0x08u) itc_consider(e, 68, 4);
    /* PN512 field wake uses vector 88 at priority 4. */
    if (IOB(0x3002A6) & IOB(0x3002A9) & 0x10u) itc_consider(e, 88, 4);
    /* IR bits 3/4/5 map to vectors 60/61/62. */
    {
        uint8_t act = IOB(0x300276) & IOB(0x300286);
        for (int b = 3; b <= 5; b++)
            if (act & (uint8_t)(1u << b))
                itc_consider(e, (uint32_t)(57 + b), 4);
    }
}

/* PN512 field activity latches its IRQ flag even while the interrupt is masked. */
void periph_nfc_field_edge(Emu *e)
{
    IOB(0x3002A9) |= 0x10;
    itc_scan(e);
}

/* RTC uses BCD 24-hour counters and cyclic interrupt vector 65. */
static uint8_t bcd_inc(uint8_t v, uint8_t wrap_after)   /* returns 0xFF on wrap */
{
    v++;
    if ((v & 0xF) > 9) v = (uint8_t)((v & 0xF0) + 0x10);
    if (v > wrap_after) return 0xFF;
    return v;
}

static const uint8_t month_days[13] = {0,0x31,0x28,0x31,0x30,0x31,0x30,0x31,0x31,0x30,0x31,0x30,0x31};

static void rtc_count_second(Emu *e)
{
    e->rtc_seconds_counted++;
    uint8_t v = bcd_inc(IOB(0x301910), 0x59);
    IOB(0x301910) = (v == 0xFF) ? 0 : v;
    if (v != 0xFF) return;
    v = bcd_inc(IOB(0x301914), 0x59);                       /* minutes */
    IOB(0x301914) = (v == 0xFF) ? 0 : v;
    if (v != 0xFF) return;
    v = bcd_inc(IOB(0x301918), 0x23);                       /* hours, 24h mode */
    IOB(0x301918) = (v == 0xFF) ? 0 : v;
    if (v != 0xFF) return;
    IOB(0x301928) = (uint8_t)((IOB(0x301928) + 1) % 7);     /* day of week */
    uint8_t mon = IOB(0x301920);
    int mi = ((mon >> 4) & 1) * 10 + (mon & 0xF);
    uint8_t dim = (mi >= 1 && mi <= 12) ? month_days[mi] : 0x31;
    if (mi == 2 && ((IOB(0x301924) & 0xF) % 4 == 0)) dim = 0x29;  /* four-year leap rule */
    v = bcd_inc(IOB(0x30191C), dim);                        /* day of month */
    IOB(0x30191C) = (v == 0xFF) ? 1 : v;
    if (v != 0xFF) return;
    v = bcd_inc(IOB(0x301920), 0x12);                       /* month */
    IOB(0x301920) = (v == 0xFF) ? 1 : v;
    if (v != 0xFF) return;
    v = bcd_inc(IOB(0x301924), 0x99);                       /* year */
    IOB(0x301924) = (v == 0xFF) ? 0 : v;
}

int periph_speed_step(int cur, int dir)
{
    static const int ladder[] = {1, 2, 5, 10, 30, 60, 120, 300, 600};
    const int n = (int)(sizeof ladder / sizeof ladder[0]);
    if (cur < 1) cur = 1;
    if (dir > 0) {
        for (int i = 0; i < n; i++) if (ladder[i] > cur) return ladder[i];
        return cur;
    }
    for (int i = n - 1; i >= 0; i--) if (ladder[i] < cur) return ladder[i];
    return cur;
}

static void rtc_tick(Emu *e, uint32_t cycles)
{
    double hz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
    double secs = (double)cycles / hz;
    double was = e->emu_secs;
    e->emu_secs += secs;
    if ((double)(long long)was != (double)(long long)e->emu_secs) fflush(stderr);
    if (e->rtc_mult > 1) secs *= e->rtc_mult;   /* --rtc-mult: accelerate game time */
    if (!(IOB(0x301908) & 2) && !(IOB(0x30190C) & 1)) {     /* running, not held */
        e->rtc_sec_frac += secs;
        while (e->rtc_sec_frac >= 1.0) { e->rtc_sec_frac -= 1.0; rtc_count_second(e); }
    }
    /* cyclic interrupt: RTCT[1:0] @0x301904 D[3:2]: 0=1/64s 1=1s 2=1min 3=1h */
    static const double period[4] = {1.0 / 64, 1.0, 60.0, 3600.0};
    double p = period[(IOB(0x301904) >> 2) & 3];
    e->rtc_irq_frac += secs;
    if (e->rtc_irq_frac >= p) {
        e->rtc_irq_frac -= p;
        IOB(0x301900) |= 1;                                 /* RTCIRQ status */
        if (IOB(0x301904) & 1)                              /* RTCIEN */
            IOB(0x300287) |= 4;                             /* FRTC cause flag */
    }
}

static void autolink_tick(Emu *e)
{
    Link *l = &e->auto_link_storage;
    /* Session state, register activity, or peer-detect IRQ can open the link. */
    int in_ir;
    /* A standby GPIO-IR receiver only reveals itself by polling inside the
     * profile's connect-driver PC range. */
    if (e->dev.ir_code_lo &&
        e->pc >= e->dev.ir_code_lo && e->pc < e->dev.ir_code_hi)
        e->ir_active_at = e->emu_secs;
    if (e->dev.nfc_pn512) {
        /* Keep an active NFC link reachable between bursts. */
        in_ir = (e->nfc.cmds > 0);
    } else {
        in_ir = (periph_ir_session_state(e) == 2) || (IOB(0x300277) & 0x08u) ||
                (e->ir_active_at > 0.0 && e->emu_secs - e->ir_active_at < 5.0);
    }

    if (!in_ir) {
        /* Keep established links open across menu pauses. */
        if (l->listening && !l->net) {
            link_close(l);
            fprintf(stderr, "[link] no peer arrived - stopped listening.\n");
        }
        return;
    }
    if (e->link) {
        /* Hubs remain open for additional devices. */
        if (l->hub && l->listening) link_auto_poll(l);
        return;
    }
    if (l->listening) { if (link_auto_poll(l)) { e->link = l; e->core_id = 0; } return; }
    if (e->emu_secs < e->auto_link_retry_at) return;
    e->auto_link_retry_at = e->emu_secs + 0.5;
    link_reset(l);
    if (link_auto_begin(l, e->auto_link_port)) { e->link = l; e->core_id = 0; }
}

/* GPIO IR sends little-endian {gap_us u16, mark_us u16} events. The 150 us
 * close and demodulator stretch preserve the firmware's bit slicing. */
#define IRG_CLOSE_US 150.0
#define IRG_DEMOD_STRETCH_US 150.0

static uint64_t irg_us2cyc(Emu *e, double us)
{
    double mclk = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
    return (uint64_t)(us * mclk / 1e6);
}

static void irg_tick(Emu *e)
{
    double mclk = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
    /* Close a TX burst after carrier toggling stops. */
    if (e->irg_tx_burst &&
        e->cycles - e->irg_tx_last_edge > irg_us2cyc(e, IRG_CLOSE_US)) {
        uint64_t markc = e->irg_tx_last_edge - e->irg_tx_start;
        uint64_t gapc  = e->irg_tx_prev_end
                       ? e->irg_tx_start - e->irg_tx_prev_end : UINT64_MAX;
        uint32_t mark_us = (uint32_t)(markc * 1e6 / mclk);
        double   gap_d   = gapc == UINT64_MAX ? 65535.0 : gapc * 1e6 / mclk;
        uint32_t gap_us  = gap_d > 65535.0 ? 65535u : (uint32_t)gap_d;
        if (mark_us > 65535u) mark_us = 65535u;
        e->irg_tx_prev_end = e->irg_tx_last_edge;
        e->irg_tx_burst = 0;
        e->irg_tx_events++;
        e->ir_active_at = e->emu_secs;
        if (e->ir_log)
            fprintf(stderr, "[ir%c] GPIO TX#%llu mark=%uus gap=%uus t=%.6f\n",
                    e->core_id ? 'B' : 'A', (unsigned long long)e->irg_tx_events,
                    mark_us, gap_us, e->emu_secs);
        if (e->link) {
            /* Collisions use the mark duration as wire time. */
            link_set_tx_byte_us(e->link, mark_us);
            link_tx(e->link, e->core_id, (uint8_t)gap_us);
            link_tx(e->link, e->core_id, (uint8_t)(gap_us >> 8));
            link_tx(e->link, e->core_id, (uint8_t)mark_us);
            link_tx(e->link, e->core_id, (uint8_t)(mark_us >> 8));
        }
    }
    while (!e->irg_rx_have && e->link && link_rx_pending(e->link, e->core_id)) {
        int b = link_rx(e->link, e->core_id);
        if (b < 0) break;
        e->irg_evt[e->irg_evt_n++] = (uint8_t)b;
        if (e->irg_evt_n == 4) {
            e->irg_evt_n = 0;
            e->irg_rx_gap_us  = (uint32_t)e->irg_evt[0] | ((uint32_t)e->irg_evt[1] << 8);
            e->irg_rx_mark_us = (uint32_t)e->irg_evt[2] | ((uint32_t)e->irg_evt[3] << 8);
            e->irg_rx_have = 1;
            /* Start after the sender's gap, accounting for time already idle. */
            uint64_t start = e->irg_v_end + irg_us2cyc(e, (double)e->irg_rx_gap_us);
            e->irg_rx_start = start > e->cycles ? start : e->cycles;
            /* Stretch the low envelope without shifting the next gap. */
            e->irg_rx_until = e->irg_rx_start +
                irg_us2cyc(e, (double)e->irg_rx_mark_us + IRG_DEMOD_STRETCH_US);
        }
    }
    if (e->irg_rx_have) {
        if (!e->irg_rx_marking && e->cycles >= e->irg_rx_start) {
            e->irg_rx_marking = 1;
            e->ir_active_at = e->emu_secs;
            e->irg_rx_events++;
            if (e->ir_log)
                fprintf(stderr, "[ir%c] GPIO RX#%llu mark=%uus gap=%uus t=%.6f\n",
                        e->core_id ? 'B' : 'A', (unsigned long long)e->irg_rx_events,
                        e->irg_rx_mark_us, e->irg_rx_gap_us, e->emu_secs);
        }
        if (e->irg_rx_marking && e->cycles >= e->irg_rx_until) {
            e->irg_rx_marking = 0;
            /* The next gap starts at the unstretched end of the optical mark. */
            e->irg_v_end = e->irg_rx_start + irg_us2cyc(e, (double)e->irg_rx_mark_us);
            e->irg_rx_have = 0;
        }
    }
}

void periph_tick(Emu *e, uint32_t cycles)
{
    rtc_tick(e, cycles);
    if (e->ir_log) link_ir_pc_sample(e);
    if (e->ir_tx_ready_at && e->cycles >= e->ir_tx_ready_at) {
        e->ir_tx_ready_at = 0;
        ir_tx_done_raise(e);
    }
    if (e->auto_link) autolink_tick(e);
    if (e->dev.nfc_pn512) { nfc_pump(e); pn512_ca_tick(e); }
    if (e->ir_log) {
        uint8_t m = periph_ir_session_state(e);
        if (m != e->ir_mode_last) {
            fprintf(stderr, "[ir%c] IR MODE %u -> %u pc=%08x t=%.6f\n",
                    e->core_id ? 'B' : 'A', e->ir_mode_last, m, e->pc, e->emu_secs);
            e->ir_mode_last = m;
        }
    }
    if (e->dev.ir_gpio)
        irg_tick(e);
    else if (!e->dev.nfc_pn512)
        ir_rx_poll(e);
    for (int x = 0; x < 6; x++) {
        uint32_t ctl = 0x300780 + (uint32_t)x * 8 + 6;
        uint8_t c = IOB(ctl);
        if (!(c & 1)) continue;                           /* PRUN */
        /* Timer clocks use MCLK-derived prescalers. */
        T16 *t = &e->t16[x];
        uint8_t ck = IOB(0x3007E0 + (uint32_t)x * 2);
        if (!(ck & 8)) continue;                          /* prescaler off */
        t->accum += cycles;
        uint32_t div = presc_div[ck & 7] << t16_extra_shift[x];
        uint32_t clocks = t->accum / div;
        t->accum -= clocks * div;
        if (clocks) t16_run(e, x, clocks > 65536 ? 65536 : clocks);
    }
    itc_scan(e);
}
