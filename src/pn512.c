#include "emu.h"
#include <string.h>

/* CommandReg bits 3-0. */
enum {
    CMD_IDLE = 0x0, CMD_MEM = 0x1, CMD_RANDOMID = 0x2, CMD_CALCCRC = 0x3,
    CMD_TRANSMIT = 0x4, CMD_NOCMDCHANGE = 0x7, CMD_RECEIVE = 0x8,
    CMD_TRANSCEIVE = 0xC, CMD_AUTOCOLL = 0xD, CMD_MFAUTHENT = 0xE,
    CMD_SOFTRESET = 0xF,
};

#define R_COMMAND   0x01
#define R_COMIEN    0x02
#define R_DIVIEN    0x03
#define R_COMIRQ    0x04
#define R_DIVIRQ    0x05
#define R_ERROR     0x06
#define R_STATUS1   0x07
#define R_STATUS2   0x08
#define R_FIFODATA  0x09
#define R_FIFOLEVEL 0x0A
#define R_CONTROL   0x0C
#define R_BITFRAME  0x0D
#define R_MODE      0x11
#define R_TXCONTROL 0x14
#define R_CRCMSB    0x21
#define R_CRCLSB    0x22
#define R_VERSION   0x37

/* ComIrq bits. */
#define IRQ_TIMER 0x01
#define IRQ_IDLE  0x10
#define IRQ_RX    0x20
#define IRQ_TX    0x40
/* DivIrq bits. */
#define IRQ_CRC   0x04

/* --nfc-log includes air traffic; --nfc-log-rf shows only air traffic. */
#define NFC_AIR(e) ((e)->nfc_log || (e)->nfc_log_rf)

static const char *regname(uint8_t a)
{
    static const char *n[0x40] = {
        [0x00] = "Page",       [R_COMMAND] = "Command",  [R_COMIEN] = "ComIEn",
        [R_DIVIEN] = "DivIEn", [R_COMIRQ] = "ComIrq",    [R_DIVIRQ] = "DivIrq",
        [R_ERROR] = "Error",   [R_STATUS1] = "Status1",  [R_STATUS2] = "Status2",
        [R_FIFODATA] = "FIFOData", [R_FIFOLEVEL] = "FIFOLevel", [0x0B] = "WaterLevel",
        [R_CONTROL] = "Control", [R_BITFRAME] = "BitFraming", [0x0E] = "Coll",
        [R_MODE] = "Mode",     [0x12] = "TxMode",        [0x13] = "RxMode",
        [R_TXCONTROL] = "TxControl", [0x15] = "TxASK",   [0x16] = "TxSel",
        [0x17] = "RxSel",      [0x18] = "RxThreshold",   [0x19] = "Demod",
        [0x1F] = "SerialSpeed", [R_CRCMSB] = "CRCMSB",   [R_CRCLSB] = "CRCLSB",
        [0x24] = "ModWidth",   [0x26] = "RFCfg",         [0x27] = "GsN",
        [0x28] = "CWGsP",      [0x29] = "ModGsP",        [0x2A] = "TMode",
        [0x2B] = "TPrescaler", [0x2C] = "TReloadH",      [0x2D] = "TReloadL",
        [R_VERSION] = "Version",
    };
    return n[a] ? n[a] : "?";
}

/* Power-up values visible to the driver. CommandReg starts idle with RX off. */
void pn512_reset(Emu *e)
{
    Pn512 *n = &e->nfc;
    memset(n->reg, 0, sizeof n->reg);
    n->reg[R_COMMAND]   = 0x20;
    n->reg[R_MODE]      = 0x3F;
    n->reg[R_TXCONTROL] = 0x80;
    n->reg[0x15]        = 0x40;   /* TxASK */
    n->reg[0x18]        = 0x84;   /* RxThreshold */
    n->reg[0x19]        = 0x4D;   /* Demod */
    n->reg[0x1F]        = 0xEB;   /* SerialSpeed: 9600 */
    n->reg[0x26]        = 0x48;   /* RFCfg */
    n->reg[0x27]        = 0x88;   /* GsN */
    n->reg[R_VERSION]   = 0x82;   /* PN512 v2 */
    n->fifo_n = 0;
    n->wr_addr = 0;
    n->rx_wait = 0;
    n->pend_n = 0;
    n->air_n = n->air_r = 0;
    n->rx_done_at = 0;
    n->tag_out_n = 0;
    n->tag_live = 0;
    /* SoftReset preserves UART state so its acknowledgement can reach the host. */
}

void pn512_power_on(Emu *e)         /* cold power-up from cpu_reset */
{
    Pn512 *n = &e->nfc;
    pn512_reset(e);
    n->txq_r = n->txq_w = 0;
    n->reply_at = 0;
    n->cmds = n->reads = n->writes = 0;
    if (!n->prng) n->prng = 0x4E464331u ^ (uint32_t)e->core_id;
}

static void reply(Emu *e, uint8_t b)
{
    Pn512 *n = &e->nfc;
    if ((unsigned)(n->txq_w - n->txq_r) >= PN512_TXQ_N) { n->txq_r++; }  /* drop oldest */
    n->txq[n->txq_w++ % PN512_TXQ_N] = b;
}

static void fifo_push(Emu *e, uint8_t b)
{
    Pn512 *n = &e->nfc;
    if (n->fifo_n < PN512_FIFO_N) n->fifo[n->fifo_n++] = b;
    else n->reg[R_ERROR] |= 0x10;                     /* BufferOvfl */
}

static uint8_t fifo_pop(Emu *e)
{
    Pn512 *n = &e->nfc;
    if (!n->fifo_n) return 0;
    uint8_t b = n->fifo[0];
    memmove(n->fifo, n->fifo + 1, (size_t)--n->fifo_n);
    return b;
}

static uint8_t prng_byte(Pn512 *n)
{
    n->prng ^= n->prng << 13; n->prng ^= n->prng >> 17; n->prng ^= n->prng << 5;
    return (uint8_t)n->prng;
}

/* ISO14443 CRC, preset per ModeReg CRCPreset. */
static uint16_t crc16_iso(uint8_t preset_sel, const uint8_t *p, int len)
{
    static const uint16_t preset[4] = {0x0000, 0x6363, 0xA671, 0xFFFF};
    uint16_t crc = preset[preset_sel & 3];
    for (int i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)(p[i] ^ crc);
        ch = (uint8_t)(ch ^ (ch << 4));
        crc = (uint16_t)((crc >> 8) ^ (ch << 8) ^ (ch << 3) ^ (ch >> 4));
    }
    return crc;
}

/* The Type 2 touch spot answers WUPA only, leaving REQA for phone downloads. */
static void spot_record(Emu *e, uint8_t *c)
{
    static const uint8_t evt[8] = {1, 2, 3, 4, 7, 8, 9, 10};
    unsigned sum = 0;
    memset(c, 0, 0x30);
    c[0] = 0x00; c[1] = 0x80;
    c[2] = 0x00; c[3] = 0x01;                 /* spot id 1 */
    c[4] = (e->touch_type > 0) ? (uint8_t)e->touch_type
         : e->nfc_probe_at     ? 1
                               : evt[prng_byte(&e->nfc) & 7];
    for (int i = 0; i < 0x2E; i++) sum += c[i];
    c[0x2E] = (uint8_t)(sum >> 8);
    c[0x2F] = (uint8_t)sum;
}

static void touchtag_arm(Emu *e)
{
    Pn512 *n = &e->nfc;
    memset(n->tag_mem, 0, sizeof n->tag_mem);
    spot_record(e, n->tag_mem + 0x10);
}

static void touchtag_say(Emu *e, const uint8_t *p, int len, int crc)
{
    Pn512 *n = &e->nfc;
    double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
    memcpy(n->tag_out, p, (size_t)len);
    n->tag_out_n = len;
    n->tag_out_crc = (uint8_t)crc;
    n->tag_out_at = e->cycles + (uint64_t)(mhz * 1e-6 * 300.0);
}

static void touchtag_hear(Emu *e, const uint8_t *f, int len)
{
    Pn512 *n = &e->nfc;
    if (e->nfc_vpeer) return;                 /* the phone owns the field */
    /* Keep pages written during the current probe. */
    if (len == 1 && (f[0] == 0x52 || (f[0] == 0x26 && e->nfc_probe_at))) {
        uint8_t atqa[2] = {0x10, 0x08};       /* ATQA 0x0810, LSB first, no CRC */
        if (!e->nfc_probe_at) touchtag_arm(e);
        n->tag_live = 1;
        e->nfc_tag_seen_at = e->cycles;
        touchtag_say(e, atqa, 2, 0);
        return;
    }
    if (!n->tag_live) return;
    e->nfc_tag_seen_at = e->cycles;
    if (len == 2 && f[0] == 0x93 && f[1] == 0x20) {        /* anticollision */
        uint8_t a[5] = {0x04, 0x54, 0x53, 0x50, 0};
        a[4] = (uint8_t)(a[0] ^ a[1] ^ a[2] ^ a[3]);       /* BCC, no CRC */
        touchtag_say(e, a, 5, 0);
    } else if (len >= 7 && f[0] == 0x93 && f[1] == 0x70) { /* SELECT */
        uint8_t sak = 0x00;                   /* plain Type 2, CRC on the air */
        touchtag_say(e, &sak, 1, 1);
    } else if (len == 2 && f[0] == 0x30) {    /* Type 2 READ: 4 pages */
        uint8_t out[16];
        for (int i = 0; i < 16; i++)
            out[i] = n->tag_mem[(((f[1] & 0x3F) * 4) + i) & 0xFF];
        touchtag_say(e, out, 16, 1);
    } else if (len == 6 && f[0] == 0xa2) {    /* Type 2 WRITE: 4 bytes to a page */
        uint8_t ack = 0x0A;                   /* 4-bit ACK, no CRC */
        memcpy(n->tag_mem + (size_t)(f[1] & 0x3F) * 4, f + 2, 4);
        if (NFC_AIR(e))
            fprintf(stderr, "[nfc%c] tag WRITE page %02x: %02x %02x %02x %02x\n",
                    e->core_id ? 'B' : 'A', f[1], f[2], f[3], f[4], f[5]);
        touchtag_say(e, &ack, 1, 0);
    } else if (len == 2 && f[0] == 0x50) {    /* HLTA: session over */
        n->tag_live = 0;
    } else if (NFC_AIR(e)) {
        fprintf(stderr, "[nfc%c] tag heard unhandled frame (%d bytes):",
                e->core_id ? 'B' : 'A', len);
        for (int i = 0; i < len && i < 12; i++) fprintf(stderr, " %02x", f[i]);
        fprintf(stderr, "%s\n", len > 12 ? " ..." : "");
    }
}

/* --nfc-probe and the N key place a store NFC point in the field. */
void pn512_probe_set(Emu *e, int on)
{
    if (on) {
        if (e->nfc_probe_at) return;
        e->nfc_probe_at = e->cycles ? e->cycles : 1;
        e->nfc_probe_next = 0;
        e->nfc_probe_until = 0;           /* the N key's field is held, not timed */
    } else {
        if (!e->nfc_probe_at) return;
        e->nfc_probe_at = 0;
        e->nfc_probe_on = 0;
        e->nfc_probe_until = 0;
        e->nfc_tag_seen_at = 0;
        e->nfc.tag_live = 0;
    }
    fprintf(stderr, "[nfc%c] store point %s the field\n",
            e->core_id ? 'B' : 'A', on ? "ENTERS" : "leaves");
}

static void rf_out(Emu *e, const uint8_t *frame, int len)
{
    if (e->nfc_peer)       pn512_rf_deliver(e->nfc_peer, frame, len);
    else if (e->nfc_vpeer) nfcpeer_rf_in(e, frame, len);
    else if (e->link)      link_nfc_tx(e->link, frame, len);
    /* The local touch spot also hears addressed tag commands. */
    touchtag_hear(e, frame, len);
}

/* Poll peer-process frames at the chip's tick cadence. */
static void rf_net_poll(Emu *e)
{
    if (!e->link || e->nfc_peer || e->nfc_vpeer) return;
    uint8_t frame[PN512_FIFO_N + 2];
    int len = link_nfc_rx(e->link, frame, (int)sizeof frame);
    if (len > 0) pn512_rf_deliver(e, frame, len);
}

/* TxLastBits gives the valid bits in the final byte; WUPA and REQA use seven. */
static void rf_send(Emu *e, int lastbits)
{
    Pn512 *n = &e->nfc;
    if (NFC_AIR(e)) {
        fprintf(stderr, "[nfc%c] RF TX %d byte%s (last %d bits):",
                e->core_id ? 'B' : 'A', n->fifo_n, n->fifo_n == 1 ? "" : "s",
                lastbits ? lastbits : 8);
        for (int i = 0; i < n->fifo_n && i < 24; i++)
            fprintf(stderr, " %02x", n->fifo[i]);
        fprintf(stderr, "%s\n", n->fifo_n > 24 ? " ..." : "");
    }
    n->rf_sent++;
    {   /* Copy before clearing the FIFO. */
        uint8_t frame[PN512_FIFO_N];
        int len = n->fifo_n;
        memcpy(frame, n->fifo, (size_t)len);
        n->fifo_n = 0;
        rf_out(e, frame, len);
    }
    n->reg[R_COMIRQ] |= IRQ_TX;
}

/* Bare activation responses set add_crc to zero. */
void pn512_rf_deliver_ex(Emu *e, const uint8_t *frame, int len, int add_crc)
{
    Pn512 *n = &e->nfc;
    if (!n->prng) pn512_power_on(e);
    /* RF activity toggles the IRQ pin even when the receiver drops the frame. */
    periph_nfc_field_edge(e);
    if (!n->rx_wait || n->fifo_n || n->air_r < n->air_n || len > PN512_AIR_N) {
        n->rf_dropped++;
        if (NFC_AIR(e))
            fprintf(stderr, "[nfc%c] RF RX %d bytes DROPPED (%s) t=%.4f cmd=%02x\n",
                    e->core_id ? 'B' : 'A', len,
                    n->fifo_n ? "fifo busy" : "not listening",
                    e->emu_secs, n->reg[R_COMMAND]);
        return;
    }
    memcpy(n->air, frame, (size_t)len);
    if (add_crc) {   /* ISO 18092 at 212 kbps uses CRC preset 0 */
        uint16_t c = crc16_iso(0, frame, len);
        n->air[len]     = (uint8_t)c;
        n->air[len + 1] = (uint8_t)(c >> 8);
    }
    n->air_n = len + (add_crc ? 2 : 0);
    n->air_r = 0;
    n->air_at = e->cycles;             /* first byte now; remaining bytes at wire rate */
    n->rx_wait = 0;
    n->rf_rcvd++;
    /* Incoming RF pushes a held frame's send time back. */
    if (n->pend_n) {
        double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
        n->pend_at = e->cycles + (uint64_t)(mhz * 1e-6 * 3000.0);
    }
    if (NFC_AIR(e)) {
        fprintf(stderr, "[nfc%c] RF RX %d byte%s t=%.4f:",
                e->core_id ? 'B' : 'A', len, len == 1 ? "" : "s", e->emu_secs);
        for (int i = 0; i < len && i < 24; i++)
            fprintf(stderr, " %02x", frame[i]);
        fprintf(stderr, "%s\n", len > 24 ? " ..." : "");
    }
    if (e->ir_fast)                    /* unit tests: no wire pacing */
        while (n->air_r < n->air_n) fifo_push(e, n->air[n->air_r++]);
    if (n->air_r >= n->air_n) { n->reg[R_COMIRQ] |= IRQ_RX; n->rx_done_at = e->cycles; }
}

static void cmd_exec(Emu *e, uint8_t v)
{
    Pn512 *n = &e->nfc;
    uint8_t cmd = v & 0x0F;
    uint8_t mode_bits = v & 0x30;             /* RcvOff | PowerDown as written */
    n->cmds++;
    if (cmd == CMD_NOCMDCHANGE)
        cmd = n->reg[R_COMMAND] & 0x0F;       /* keep the running command */
    if (e->nfc_log)
        fprintf(stderr, "[nfc%c] cmd %02x (mode %02x)\n",
                e->core_id ? 'B' : 'A', cmd, mode_bits);
    switch (cmd) {
    case CMD_IDLE:
        /* Idle aborts reception, held transmission, and in-flight data. */
        n->rx_wait = 0;
        n->pend_n = 0;
        n->air_n = n->air_r = 0;
        break;
    case CMD_MEM:
        if (!n->fifo_n)
            for (int i = 0; i < 25; i++) fifo_push(e, n->buf25[i]);
        else {
            for (int i = 0; i < 25 && n->fifo_n; i++) n->buf25[i] = fifo_pop(e);
        }
        cmd = CMD_IDLE;
        break;
    case CMD_RANDOMID:
        /* Mix in call time before generating an ID. */
        n->prng ^= (uint32_t)e->cycles | 1u;
        for (int i = 0; i < 10; i++) n->buf25[i] = prng_byte(n);
        cmd = CMD_IDLE;
        break;
    case CMD_CALCCRC: {
        uint16_t c = crc16_iso(n->reg[R_MODE], n->fifo, n->fifo_n);
        n->fifo_n = 0;
        n->reg[R_CRCMSB] = (uint8_t)(c >> 8);
        n->reg[R_CRCLSB] = (uint8_t)c;
        n->reg[R_DIVIRQ] |= IRQ_CRC;
        break;                                /* runs until told otherwise */
    }
    case CMD_TRANSMIT:
        rf_send(e, n->reg[R_BITFRAME] & 7);
        n->reg[R_COMIRQ] |= IRQ_IDLE;
        cmd = CMD_IDLE;
        break;
    case CMD_RECEIVE:
    case CMD_AUTOCOLL:
        n->rx_wait = 1;
        break;
    case CMD_TRANSCEIVE:
        /* With its field off, Transceive starts in target/listen mode. */
        if (!(n->reg[R_TXCONTROL] & 0x03)) n->rx_wait = 1;
        break;
    case CMD_MFAUTHENT:
        break;
    case CMD_SOFTRESET:
        pn512_reset(e);
        return;
    default:
        break;
    }
    n->reg[R_COMMAND] = (uint8_t)(mode_bits | cmd);
}

static void reg_write(Emu *e, uint8_t a, uint8_t v)
{
    Pn512 *n = &e->nfc;
    n->writes++;
    if (e->nfc_log)
        fprintf(stderr, "[nfc%c] wr %-11s(%02x) = %02x\n",
                e->core_id ? 'B' : 'A', regname(a), a, v);
    switch (a) {
    case R_COMMAND:
        cmd_exec(e, v);
        break;
    case R_COMIRQ:                            /* Set1 semantics */
        if (v & 0x80) n->reg[a] |= (uint8_t)(v & 0x7F);
        else          n->reg[a] &= (uint8_t)~(v & 0x7F);
        break;
    case R_DIVIRQ:
        if (v & 0x80) n->reg[a] |= (uint8_t)(v & 0x7F);
        else          n->reg[a] &= (uint8_t)~(v & 0x7F);
        break;
    case R_FIFODATA:
        fifo_push(e, v);
        break;
    case R_FIFOLEVEL:
        if (v & 0x80) {
            n->fifo_n = 0;
            n->reg[R_ERROR] &= (uint8_t)~0x10;
            /* A FIFO flush also ends reception so peer data cannot splice into
             * the next outbound frame. */
            n->rx_wait = 0;
            n->air_n = n->air_r = 0;
        }
        break;
    case R_CONTROL:
        /* bit6 TStartNow: the drivers leave TReload at 0, and a zero reload
         * underflows at once, so the timer fires as it starts */
        if (v & 0x40) n->reg[R_COMIRQ] |= IRQ_TIMER;
        if (v & 0x20)                         /* WrNFCIDtoFIFO */
            for (int i = 0; i < 10; i++) fifo_push(e, n->buf25[i]);
        n->reg[a] = (uint8_t)(v & 0x1F);      /* bits 7-5 self-clear */
        break;
    case R_BITFRAME:
        n->reg[a] = (uint8_t)(v & 0x7F);      /* StartSend self-clears */
        if ((v & 0x80) && (n->reg[R_COMMAND] & 0x0F) == CMD_TRANSCEIVE) {
            if (n->reg[R_TXCONTROL] & 0x03) {
                rf_send(e, v & 7);
                n->rx_wait = 1;               /* transceive now awaits the answer */
            } else {
                /* Hold the frame while listening. Random delay prevents
                 * lockstepped peers from livelocking. */
                double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
                n->pend_n = n->fifo_n;
                memcpy(n->pend, n->fifo, (size_t)n->fifo_n);
                n->fifo_n = 0;
                /* Responses use TADT; initial probes wait for randomized quiet air. */
                if (n->rx_done_at &&
                    e->cycles - n->rx_done_at < (uint64_t)(mhz * 1e-6 * 5000.0))
                    n->pend_at = e->cycles + (uint64_t)(mhz * 1e-6 * 150.0);
                else
                    n->pend_at = e->cycles + (uint64_t)(mhz * 1e-6
                               * (1000.0 + 300.0 * (prng_byte(n) & 7)));
                n->rx_wait = 1;
                if (NFC_AIR(e))
                    fprintf(stderr, "[nfc%c] RF CA-hold %d bytes, receiver live\n",
                            e->core_id ? 'B' : 'A', n->pend_n);
            }
        }
        break;
    case R_ERROR:
    case R_STATUS1:
    case R_STATUS2:
        break;                                /* read-only / computed */
    default:
        n->reg[a] = v;
        break;
    }
}

static uint8_t reg_read(Emu *e, uint8_t a)
{
    Pn512 *n = &e->nfc;
    uint8_t v;
    n->reads++;
    switch (a) {
    case R_FIFODATA:  v = fifo_pop(e); break;
    case R_FIFOLEVEL: v = (uint8_t)n->fifo_n; break;
    case R_STATUS1:
        /* CalcCRC completes immediately; the timer never reports running. */
        v = (uint8_t)(0x20 | ((!n->reg[R_CRCMSB] && !n->reg[R_CRCLSB]) ? 0x40 : 0));
        break;
    case R_STATUS2: {
        /* ModemState 5 means a transceive is waiting for data. */
        uint8_t cmd = n->reg[R_COMMAND] & 0x0F;
        v = (cmd == CMD_TRANSCEIVE || cmd == CMD_RECEIVE) ? 0x05 : 0x00;
        break;
    }
    default: v = n->reg[a]; break;
    }
    if (e->nfc_log)
        fprintf(stderr, "[nfc%c] rd %-11s(%02x) -> %02x\n",
                e->core_id ? 'B' : 'A', regname(a), a, v);
    return v;
}

/* Receive one byte from the host UART. */
void pn512_host_byte(Emu *e, uint8_t b)
{
    Pn512 *n = &e->nfc;
    if (!n->prng) pn512_power_on(e);          /* supports direct unit-test setup */
    if (n->wr_addr) {
        uint8_t a = (uint8_t)(n->wr_addr & 0x3F);
        n->wr_addr = 0;
        reg_write(e, a, b);
        reply(e, a);                          /* write ack: the address, echoed */
    } else if (b & 0x80) {
        reply(e, reg_read(e, b & 0x3F));
    } else {
        n->wr_addr = 0x100 | (b & 0x3F);
    }
}

/* Stream incoming frames at 212 kbps and send held frames after quiet air. */
void pn512_ca_tick(Emu *e)
{
    Pn512 *n = &e->nfc;
    rf_net_poll(e);
    /* The timeout prevents an unanswered probe from leaving the field active. */
    if (e->nfc_probe_until && e->cycles >= e->nfc_probe_until) {
        e->nfc_probe_until = 0;
        e->nfc_tag_seen_at = 0;
        pn512_probe_set(e, 0);
    }
    if (e->nfc_probe_at && e->cycles >= e->nfc_probe_at) {
        if (!e->nfc_probe_on) {
            e->nfc_probe_on = 1;
            touchtag_arm(e);
            if (NFC_AIR(e))
                fprintf(stderr, "[nfc%c] probe: store-point field on, tag armed "
                        "(type %d) t=%.4f\n", e->core_id ? 'B' : 'A',
                        n->tag_mem[0x14], e->emu_secs);
        }
        if (e->cycles >= e->nfc_probe_next) {
            double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
            e->nfc_probe_next = e->cycles + (uint64_t)(mhz * 0.25);
            periph_nfc_field_edge(e);
        }
    }
    if (e->nfc_vpeer) nfcpeer_tick(e);
    if (n->tag_out_n && e->cycles >= n->tag_out_at) {
        int len = n->tag_out_n;
        n->tag_out_n = 0;
        pn512_rf_deliver_ex(e, n->tag_out, len, n->tag_out_crc);
    }
    if (n->air_r < n->air_n) {
        double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
        uint64_t byte_cyc = (uint64_t)(mhz * 8.0 / 212000.0);
        while (n->air_r < n->air_n && e->cycles >= n->air_at) {
            fifo_push(e, n->air[n->air_r++]);
            n->air_at += byte_cyc;
        }
        if (n->air_r >= n->air_n) { n->reg[R_COMIRQ] |= IRQ_RX; n->rx_done_at = e->cycles; }
    }
    if (!n->pend_n || e->cycles < n->pend_at) return;
    if (n->air_r < n->air_n) {         /* mid-reception: the air is not quiet */
        n->pend_at = n->air_at + (uint64_t)((e->cmu.mclk_hz > 0 ?
                     e->cmu.mclk_hz : e->dev.osc3_hz) * 1e-6 * 500.0);
        return;
    }
    if (NFC_AIR(e)) {
        fprintf(stderr, "[nfc%c] RF TX (CA) %d bytes t=%.4f:",
                e->core_id ? 'B' : 'A', n->pend_n, e->emu_secs);
        for (int i = 0; i < n->pend_n && i < 24; i++)
            fprintf(stderr, " %02x", n->pend[i]);
        fprintf(stderr, "%s\n", n->pend_n > 24 ? " ..." : "");
    }
    n->rf_sent++;
    rf_out(e, n->pend, n->pend_n);
    n->pend_n = 0;
    n->reg[R_COMIRQ] |= IRQ_TX;               /* rx_wait stays: awaiting reply */
}

void pn512_report(Emu *e, FILE *f)
{
    Pn512 *n = &e->nfc;
    if (!n->cmds && !n->reads && !n->writes) return;
    fprintf(f, "[nfc%c] reg writes %llu, reads %llu, commands %llu; "
               "RF frames sent %llu, heard %llu, dropped %llu\n",
            e->core_id ? 'B' : 'A',
            (unsigned long long)n->writes, (unsigned long long)n->reads,
            (unsigned long long)n->cmds, (unsigned long long)n->rf_sent,
            (unsigned long long)n->rf_rcvd, (unsigned long long)n->rf_dropped);
}

void pn512_rf_deliver(Emu *e, const uint8_t *frame, int len)
{
    pn512_rf_deliver_ex(e, frame, len, 1);
}
