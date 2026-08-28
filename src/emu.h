#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

/* device.c defines profiles; each Emu selects one at startup. */
typedef struct DeviceProfile {
    /* Per-device S1C33E07 memory map, ordered for the decoder's hot path. */
    uint32_t rom_base,    rom_size;
    uint32_t a0ram_base,  a0ram_size;
    uint32_t io_base,     io_size;
    uint32_t ivram_base,  ivram_size;
    uint32_t dstram_base, dstram_size;
    uint32_t lcd_cmd_addr, lcd_data_addr;
    uint32_t io_end;
    uint32_t ir_base;          /* IR register block: 8 registers from here */
    /* Session state is either an absolute byte or [*ir_ctx_ptr + IR_CTX_STATE].
     * A nonzero ir_mode_flag also enables receive-side session gating. */
    uint32_t ir_mode_flag;
    uint32_t ir_ctx_ptr;
    /* A separate presence flag keeps address zero valid. */
    bool     has_sleep_flag;
    uint32_t sleep_flag;
    /* At accelerated clocks, keep sleep off if waking consumes the button press. */
    bool     wake_press_lost;
    bool     nfc_pn512;         /* B10 UART carries the PN512 host protocol */
    /* PCs that raise the bingo/gashapon NFC field and count the play. */
    uint32_t bingo_open_pc;
    uint32_t bingo_play_pc;
    /* Return PCs bound the full touch routine; a timed tap can end mid-animation. */
    uint32_t bingo_done_pc[2];
    /* GPIO-IR uses a carrier output and an active-low demodulated input. */
    bool     ir_gpio;
    uint8_t  ir_gpio_tx_bit;   /* P0 bit the carrier is toggled on */
    uint8_t  ir_gpio_rx_bit;   /* P0 bit the receive envelope is read on */
    /* Connect-driver range used to detect a polling-only GPIO-IR receiver. */
    uint32_t ir_code_lo, ir_code_hi;
    bool     flash_top_boot;    /* parameter sectors are in the top 64 KiB */
    bool     debug_strap_high;  /* debug mode is selected by P06 high */
    /* TTBR reset value inside this device's NOR. */
    uint32_t ttbr_reset;
    double   osc3_hz;           /* OSC3 crystal frequency */
    const char *name;          /* --device selects by this */
    const char *title;         /* window title and log banner */
} DeviceProfile;

/* P's defaults shared by several profiles. */
#define PS_ROM_BASE     0x02000000u
#define PS_ROM_SIZE     0x00800000u   /* 8 MB NOR (P's) */
#define PS_A0RAM_BASE   0x00000000u
#define PS_A0RAM_SIZE   0x00008000u   /* 32 KiB map for A0RAM and relocated IVRAM */
#define PS_IVRAM_BASE   0x00080000u
#define PS_IVRAM_SIZE   0x00003000u   /* 12KB */
#define PS_DSTRAM_BASE  0x00084000u
#define PS_DSTRAM_SIZE  0x00001000u   /* 4KB */
#define PS_IO_BASE      0x00300000u
#define PS_IO_END       0x00302000u
#define PS_LCD_CMD_ADDR  0x00600000u
#define PS_LCD_DATA_ADDR 0x00600001u
#define PS_IR_BASE       0x00300B10u
#define PS_OSC3_HZ       18432000.0

/* Session-state offset used with ir_ctx_ptr. */
#define IR_CTX_STATE     0x4E4u

/* Compile-time storage ceilings; device_check() rejects larger profiles. */
#define A0RAM_MAX   0x00008000u
#define IVRAM_MAX   0x00003000u
#define DSTRAM_MAX  0x00001000u
#define IORAM_MAX   0x00002000u

/* device.c */
const DeviceProfile *device_find(const char *name);  /* NULL if no such device */
const DeviceProfile *device_at(size_t i);            /* walk the table; NULL past the end */
const DeviceProfile *device_default(void);           /* P's */
void device_list(FILE *f);                           /* --device help */
int  device_check(const DeviceProfile *d, FILE *f);  /* 0 if it does not fit the ceilings */

int savepath_default(char *out, size_t outsz, const char *rompath,
                     const DeviceProfile *dev);
int savepath_legacy_default(char *out, size_t outsz, const char *rompath,
                            const DeviceProfile *dev);
int savepath_mkdirs(const char *rompath, const DeviceProfile *dev);
int savepath_migrate_legacy(const char *legacy_sav, const char *savpath);

#define GRAM_W 132
#define GRAM_H 162
/* P's visible panel is 128 x 128. */
#define PANEL_W 128
#define PANEL_H 128
#define PANEL_SCALE 3 /* play and link windows draw the LCD at 3x */
#define STRIP_H 72    /* A/B/C button strip below the LCD */

typedef struct Lcd {
    uint16_t gram[GRAM_H][GRAM_W];
    uint8_t  cmd;              /* current command */
    int      argc;             /* data bytes received for current command */
    uint8_t  args[16];
    /* window state */
    int xs, xe, ys, ye, cx, cy;
    uint8_t  madctl, colmod;
    bool     sleep_out, disp_on, inverted;
    uint8_t  gamma_pos[16], gamma_neg[16];
    int      gp_n, gn_n;
    uint8_t  pwr_c0[2], pwr_c2[1], vcom_c5[1], vcom_c6[1];
    int      c0_n, c2_n, c5_n, c6_n;
    uint8_t  pix_buf[3];       /* pending pixel bytes (2 = RGB565, 3 = RGB666 default) */
    int      pix_n;
    uint64_t ramwr_bytes;
    uint64_t cmd_count;
    bool     log;              /* --lcd-log */
    FILE    *logf;
    uint64_t frame_marker;     /* bumped on DISPON / big RAMWR bursts for frontend */
} Lcd;

typedef struct Cmu {
    uint8_t  unlocked;         /* 0x301B24 == 0x96 */
    uint32_t sccr;             /* 0x301B08 */
    uint32_t pllc;             /* 0x301B0C */
    uint32_t opt;              /* 0x301B14 */
    double   osc3_hz;          /* crystal, default 20e6, --osc3 */
    double   mclk_hz;          /* computed */
} Cmu;

typedef struct T16 {
    uint16_t counter;
    uint8_t  a_fired;          /* compare-A already fired this period (edge detect) */
    uint32_t accum;            /* cycle accumulator vs prescaler ratio */
    double   frac;             /* fractional counts for external-clock mode */
} T16;

/* PN512 uses six-bit register addresses over B10. Writes are address/data
 * pairs; reads set address bit 7. */
#define PN512_FIFO_N 64
#define PN512_TXQ_N  64
/* Air frames can exceed the FIFO because the host drains it while receiving. */
#define PN512_AIR_N  272
typedef struct Pn512 {
    uint8_t  reg[0x40];
    uint8_t  fifo[PN512_FIFO_N];
    int      fifo_n;
    uint8_t  buf25[25];        /* internal buffer: Mem / RandomID / NFCID */
    int      wr_addr;          /* 0x100 | addr while a write's data byte is due */
    uint8_t  txq[PN512_TXQ_N]; /* replies waiting to cross the UART to the host */
    int      txq_r, txq_w;     /* ring; live index = counter % PN512_TXQ_N */
    uint64_t reply_at;         /* cycle the next queued reply may be delivered */
    uint32_t prng;             /* RandomID state; reseeded with e->cycles per use */
    /* Receive, AutoColl, and active Transceive commands arm RF reception. */
    uint8_t  rx_wait;
    /* Frame held by RF collision avoidance while the receiver remains live. */
    uint8_t  pend[PN512_FIFO_N];
    int      pend_n;
    uint64_t pend_at;
    /* Stream incoming frames at wire rate; long frames exceed the FIFO and are
     * read while arriving. RxIRq fires after the final byte. */
    uint8_t  air[PN512_AIR_N + 2];
    int      air_n, air_r;
    uint64_t air_at;
    uint64_t rx_done_at;            /* cycles when the last reception finished */
    uint64_t cmds, reads, writes;   /* counters for the session report */
    uint64_t rf_sent, rf_rcvd, rf_dropped;
    /* The passive Type 2 touch tag answers WUPA, not the phone path's REQA. */
    uint8_t  tag_mem[0x100];        /* pages 0-63; touch data uses pages 4-15 */
    uint8_t  tag_out[20];           /* one pending reply */
    int      tag_out_n;
    uint8_t  tag_out_crc;
    uint64_t tag_out_at;
    uint8_t  tag_live;              /* a WUPA opened a touch session */
} Pn512;

/* --nfc-inject models the phone target; the 4U remains the NFC-DEP initiator.
 * DEP permits one pending response. Payload buffers must hold a complete item. */
#define NFCPEER_MAX 65536
typedef struct NfcPeer {
    uint8_t *payload;          /* the TAMAGO blob to hand over */
    int      payload_n;
    int      state;            /* protocol phase (nfcpeer.c) */
    int      tap;              /* 0 = handshake session, 1 = download session */
    int      llcp;             /* peer advertised LLCP in its ATR_REQ general bytes */
    uint8_t  nfcid3[10];
    uint8_t  did;
    uint8_t  pni;              /* DEP packet number, 2 bits */
    uint8_t  uid[4];           /* NFCID1t presented during Type-A activation */
    int      activated;        /* Type-A activation finished (SAK sent) */
    uint8_t  out[PN512_AIR_N];
    int      out_n;
    int      out_crc;          /* 0 = the reply goes out without an air CRC */
    uint64_t out_at;           /* cycle the reply is delivered; 0 = nothing due */
    /* Our LLCP SAP is 32; the tama's SNEP server uses SAP 4. */
    uint8_t  ll_peer_sap;      /* the tama's SSAP, learned from its CONNECT */
    uint8_t  ns, nr;           /* our LLCP send/receive sequence, 4 bits each */
    /* SNEP PUT payload and reply reassembly; each I-PDU carries 125 bytes. */
    uint8_t  tx[NFCPEER_MAX];
    int      tx_n, tx_sent;
    uint8_t  rx[NFCPEER_MAX];
    int      rx_n, rx_want;
    uint64_t frames_in, frames_out;
    int      log;
} NfcPeer;

/* Shared IR bus. rxq[i] receives every participant's bytes except its own.
 * Head and tail are monotonic ring counters. */
#define LINK_FIFO_N 8192   /* burst headroom: a visit moves ~165 KB */
#define LINK_PEERS_MAX 4   /* the in-game PLAY menu offers at most 3 others */
/* net-mode wire record: sender u32 | byte | rsv | dur_us u16 | t_us u64 */
#define LINK_REC_N     16
#define LINK_NFC_Q      8   /* complete NFC frames buffered from the wire */
#define LINK_PEND_MAX  512 /* records held until their wire-time due */
#define LINK_COLL_HIST 64  /* recently released transmissions kept for overlap */
#define LINK_ORG_N      8  /* clock-gap estimates, with spare slots for reconnects */
/* Jitter and collision-reorder window. reorder_late reports insufficient lead. */
#define LINK_LEAD_US 1500
/* Settle time comes from each record's byte time. */
typedef struct Link {
    uint8_t  rxq[LINK_PEERS_MAX][LINK_FIFO_N];
    unsigned head[LINK_PEERS_MAX], tail[LINK_PEERS_MAX];
    int      npeers;                 /* participants on the bus; 2 unless raised */
    uint64_t bytes_ab, bytes_ba;     /* lifetime counters (verification) */
    unsigned overflows;              /* dropped bytes (should stay 0 in lockstep) */
    /* Socket mode uses one hub to relay the shared bus among guests. */
    int       net;
    int       hub;                   /* 1 = we accept guests and relay between them */
    uintptr_t peer[LINK_PEERS_MAX];  /* live sockets; guests use peer[0] only */
    int       nsock;                 /* how many of peer[] are connected */
    uintptr_t listener;              /* pending listening socket (hub/auto-link) */
    int       listening;
    /* Auto-link joins an existing hub asynchronously so the SDL loop never
     * waits in connect(). link_auto_poll resolves this socket. */
    uintptr_t joiner;
    int       joining;
    int       join_port;
    /* ---- collision model (socket mode only) ----
     * Sender IDs and host-time intervals let receivers corrupt overlapping IR
     * transmissions that TCP would otherwise deliver cleanly. */
    uint32_t  my_id;                 /* unique per process; 0 until net starts */
    unsigned  tx_byte_us;            /* how long our current byte occupies the wire */
    uint64_t  seen_t0[LINK_COLL_HIST], seen_t1[LINK_COLL_HIST];
    uint32_t  seen_id[LINK_COLL_HIST];
    int       seen_n;                /* ring of recently released transmissions */
    uint8_t   pend[LINK_PEND_MAX][LINK_REC_N];
    int       pend_n;                /* held briefly so late arrivals can collide */
    uint8_t   part[LINK_PEERS_MAX][LINK_REC_N];
    int       part_n[LINK_PEERS_MAX];/* a record split across two recv()s */
    uint64_t  collisions;            /* corrupted bytes (verification) */
    /* ---- wire-time delivery (socket mode only) ----
     * Hold each record until its sender timestamp plus the per-origin clock
     * offset and LINK_LEAD_US. */
    uint32_t  org_id[LINK_ORG_N];
    int64_t   org_off[LINK_ORG_N];   /* current gap estimate, us */
    uint64_t  org_upd[LINK_ORG_N];   /* release-clock time of the last sample */
    int       org_n;
    uint64_t  released_t_max;        /* newest wire time already committed */
    uint64_t  reorder_late;          /* records that arrived AFTER a younger one
                                      * was released: the reorder horizon broke */
    uint64_t  test_now;              /* scripted release clock (unit tests) */
    int       test_now_set;
    /* ---- diagnostics only; these fields do not affect delivery ---- */
    unsigned  backlog_max;           /* deepest the rx FIFO ever got, in bytes */
    int       pend_max;              /* high-water of the settle queue */
    uint64_t  send_fails;            /* records a socket refused - those bytes are lost */
    /* ---- NFC frames over the same socket ----
     * Typed byte records preserve frame boundaries across partial reads. NFC
     * bypasses the IR timing model; pn512.c handles RF collision avoidance. */
    uint8_t   nfc_asm[LINK_PEERS_MAX][PN512_FIFO_N + 2];
    int       nfc_asm_n[LINK_PEERS_MAX];   /* frame being reassembled, per socket */
    uint8_t   nfc_q[LINK_NFC_Q][PN512_FIFO_N + 2];
    int       nfc_q_len[LINK_NFC_Q];
    unsigned  nfc_q_head, nfc_q_tail;      /* complete frames waiting to be read */
    uint64_t  nfc_frames_tx, nfc_frames_rx, nfc_frames_dropped;
} Link;

/* NFC frame relay over the socket transport (net mode only). */
void link_nfc_tx(Link *l, const uint8_t *frame, int len);
int  link_nfc_rx(Link *l, uint8_t *out, int max);   /* 0 = nothing waiting */

typedef struct Emu {
    /* CPU */
    uint32_t r[16];
    uint32_t sp, alr, ahr, ttbr, idir, dbbr;
    uint32_t pc;
    uint8_t  f_n, f_z, f_v, f_c, f_ie;
    uint8_t  il;
    uint32_t psr_rest;         /* unmodeled PSR bits round-trip */
    /* ext prefix state */
    int      ext_count;
    uint32_t ext_val[2];
    /* run state */
    bool     halted;           /* slp/halt */
    bool     halt_hard;        /* halt: only int wakes; slp: auto-wake allowed */
    uint64_t wake_at;          /* cycle for slp auto-wake */
    uint64_t cycles;
    bool     stopped;          /* fatal: unknown opcode etc. */
    char     stop_reason[128];

    /* Selected at startup and read-only afterward. */
    DeviceProfile dev;

    /* Arrays use the storage ceilings; dev supplies the mapped bounds. */
    uint8_t *rom;              /* dev.rom_size bytes: the whole NOR image */
    uint8_t  a0ram[A0RAM_MAX];
    uint8_t  ivram[IVRAM_MAX];
    uint8_t  dstram[DSTRAM_MAX];
    uint8_t  ioram[IORAM_MAX];         /* register-RAM backing for peripherals */

    /* peripherals */
    Cmu      cmu;
    Lcd      lcd;
    T16      t16[6];
    uint32_t pending_irq;      /* 0 = none, else vector number */
    uint8_t  pending_level;
    bool     wake_req;         /* flag&enable exists: clears standby even if masked */
    uint64_t last_tick;
    uint8_t  btn_mask;         /* bit0=P00(A) bit1=P01(B) bit2=P02(C), 1 = pressed */
    bool     no_sleep;         /* clear the profile's idle-sleep flag */
    bool     stay_awake;       /* Tier1: force panel on; Tier2: block fw idle-sleep */
    uint32_t watch_lo, watch_hi; /* --watch-io: log unique (pc,addr) reads in range */
    /* --watch-ram logs changed A0RAM writes in up to four ranges. */
    uint32_t rwatch_lo[4], rwatch_hi[4];
    int      rwatch_n;
    int      rwatch_budget;
    /* --btn-log records input edges and the firmware's action gates. */
    bool     btn_log;
    uint32_t btn_cnt_addr;     /* A0RAM: 3 press counters (A,B,C); 0 = unknown */
    uint32_t btn_gate_addr;    /* DSTRAM: modal-routine gate byte; 0 = unknown */
    /* --watch-flash logs NOR writes in a range with the writing PC. */
    uint32_t fwatch_lo, fwatch_hi;
    int      fwatch_budget;
    double   rtc_sec_frac;     /* fractional seconds toward next RTC count */
    double   rtc_irq_frac;     /* fractional period toward next cyclic RTC irq */
    double   emu_secs;         /* true emulated wall time (mclk-aware) */
    uint32_t t2_attacks;       /* bumped each time the fw (re)starts the T2 tone */
    double   t2_freq, t2_duty; /* pitch/timbre latched at the last attack */

    /* Cycle-stamped T0 piezo events; output is audible only when 0 < CRA < CRB. */
    #define TONE_EV_N 1024
    struct ToneEv { uint64_t cyc; float freq; uint8_t on; } tone_ev[TONE_EV_N];
    unsigned tone_ev_w;        /* total events ever written (index mod TONE_EV_N) */
    uint8_t  tone_on;          /* current computed state (dedup) */
    float    tone_freq;

    /* NOR flash (AMD/JEDEC command set: KH29LV320DT / MX29LV640 class) */
    uint64_t rtcirq_clears;    /* fw consumed 1/64s ticks */
    uint64_t rtc_seconds_counted;
    uint64_t halt_wakes, irq_taken[80];
    uint8_t  wake_flags_snapshot[4];  /* ITC flag&enable bytes at last wake */
    uint64_t t16_fires[6];     /* compare-B flag sets per timer */
    int      flash_state;      /* 0 idle, 1 AA, 2 AA+55, 3 program-next, 4 80, 5 80+AA, 6 80+AA+55 */
    bool     flash_autoselect; /* reads return mfr/device ID until F0 reset */
    bool     flash_dirty;      /* any program/erase happened (drives .sav writeback) */
    uint64_t flash_programs, flash_erases, flash_delay_hits;
    /* Busy flash reads return DQ7 complement and DQ6 toggle status. */
    int      flash_busy;       /* remaining status reads before op reports done */
    uint16_t flash_op_data;    /* final data of the word being programmed (for DQ7) */
    bool     flash_toggle;     /* DQ6 toggle state */

    /* diagnostics */
    bool     trace;
    FILE    *tracef;
    bool     io_log;
    bool     ir_log;           /* --ir-log: trace the IR (0x300B1x) modulator/receiver */
    uint64_t ir_tx_symbols;    /* symbols the fw fed the modulator */
    uint64_t ir_rx_symbols;    /* symbols delivered into the receiver */
    uint64_t ir_last_edge_cyc; /* cycle of the last B13 carrier on/off edge */
    uint64_t ir_edges;         /* carrier transitions seen */
    /* --ir-log samples PCs while IR is active. */
    uint32_t pc_hist[96], pc_hits[96];
    int      pc_hist_n;
    uint64_t pc_samples;
    bool     tone_log;         /* --tone-log: trace T2 tone register writes */
    bool     debug_strap;      /* --debug-strap: P06 low = fw debug/test mode */
    int      rtc_mult;         /* --rtc-mult N: RTC runs N x faster (aging shortcut) */
    uint32_t last_io_read;     /* last polled IO/unmapped address */
    uint64_t io_writes, lcd_writes;
    /* --pc-trace collapses repeated entries when dumping this ring. */
    #define TRACE_RING_N 8192
    #define TRACE_RING_M (TRACE_RING_N - 1)
    uint32_t trace_ring[TRACE_RING_N];
    int      trace_ring_pos;

    /* IrDA link; NULL in standalone mode. */
    struct Link *link;
    int          core_id;      /* 0 = A, 1 = B */

    /* Auto-link opens the port when firmware enters connect mode. */
    bool     auto_link;
    int      auto_link_port;
    double   auto_link_retry_at;
    double   ir_active_at;     /* emu_secs of the last IR-hardware touch (any feature) */
    /* IR bytes take ten bit-times; --ir-fast disables this pacing. */
    bool     ir_fast;
    uint64_t ir_tx_ready_at;   /* cycle the current TX byte finishes shifting out */
    Pn512    nfc;              /* the reader itself, when dev.nfc_pn512 is set */
    bool     nfc_log;          /* --nfc-log: decoded register accesses and commands */
    bool     nfc_log_rf;       /* --nfc-log-rf: air traffic without register accesses */
    int      touch_type;       /* --touch-type 1..10 pins the touch spot's event
                                * byte; 0 (default) rolls one per tap */
    uint64_t nfc_probe_at;     /* cycle a store NFC point enters the field; 0 = absent */
    uint64_t nfc_probe_next;   /* next field pulse */
    uint64_t nfc_probe_until;  /* auto-touch field removal cycle; 0 = held */
    uint64_t nfc_tag_seen_at;  /* cycle of the last frame handled by the touch tag */
    int      nfc_probe_on;     /* field announced + tag armed */
    bool     auto_touch;       /* opening a card touches the point; --no-auto-touch off */
    /* In --link mode, each NFC core transmits to the other. */
    struct Emu *nfc_peer;
    NfcPeer *nfc_vpeer;        /* --nfc-inject: the phone, modeled in process */
    uint64_t ir_rx_next_at;    /* earliest cycle the next RX byte may be delivered */
    uint64_t ir_overruns;      /* bytes lost because the fw stopped draining */
    /* GPIO IR carries four-byte {gap_us, mark_us} envelope events. */
    uint8_t  irg_port_latch;   /* last value the fw wrote to the port data reg */
    int      irg_tx_burst;     /* nonzero while the fw is toggling the TX bit */
    uint64_t irg_tx_start;     /* cycle the current burst's first edge landed */
    uint64_t irg_tx_last_edge; /* cycle of the burst's most recent edge */
    uint64_t irg_tx_prev_end;  /* cycle the previous burst ended (gap base) */
    uint8_t  irg_evt[4];       /* inbound event being assembled from the FIFO */
    int      irg_evt_n;        /* bytes of it collected so far */
    int      irg_rx_have;      /* a decoded event is waiting to be replayed */
    uint32_t irg_rx_gap_us, irg_rx_mark_us;
    uint64_t irg_rx_start;     /* cycle the replayed mark begins */
    uint64_t irg_rx_until;     /* cycle it ends (envelope reads low in between) */
    int      irg_rx_marking;   /* envelope is currently low (carrier present) */
    uint64_t irg_v_end;        /* cycle the last replayed mark ended (gap base) */
    uint64_t irg_tx_events, irg_rx_events;   /* lifetime counters, end report */
    uint64_t ir_rx_dropped_idle; /* peer bytes discarded: this core was not in IR mode */
    uint8_t  ir_mode_last;     /* --ir-log: last periph_ir_session_state(), for logging its edges */
    /* --ir-log records distinct 16-byte PC buckets after an IR session ends. */
    int      post_ir_trace;    /* remaining buckets to log, 0 = off */
    uint32_t post_ir_last;     /* last bucket logged (consecutive dedup) */
    /* --call-trace follows calls nested beneath up to four selected ranges. */
    #define CALL_RANGES 4
    /* PC hooks live in cpu_step so they also run under --link. */
    #define PC_HOOKS 24
    uint32_t    pc_hook[PC_HOOKS];
    const char *pc_hook_name[PC_HOOKS];
    uint8_t     pc_hook_ring[PC_HOOKS];  /* also dump the 64-entry trace ring */
    int         n_pc_hook;
    uint32_t call_lo[CALL_RANGES], call_hi[CALL_RANGES];
    int      call_nranges;     /* 0 -> tracing off */
    int      call_depth;       /* frames currently inside a traced call */
    int      call_budget;      /* remaining lines to print (runaway guard) */
    Link     auto_link_storage;
} Emu;

typedef enum StateResult {
    STATE_NONE,
    STATE_LOADED,
    STATE_REJECTED,
    STATE_IO_ERROR
} StateResult;

/* NFC and IR traffic use the same socket transport but incompatible wire
 * protocols. Keep their automatic rendezvous ports separate; an explicit
 * --ir-port always wins. */
static inline int auto_link_default_port(const DeviceProfile *dev, int port,
                                         bool port_set)
{
    return !port_set && dev && dev->nfc_pn512 ? port + 1 : port;
}

/* Network polling belongs to host time. The state snapshot intentionally does
 * not carry this timestamp across launches. */
static inline bool host_interval_due(uint64_t now_ms, uint64_t *last_ms,
                                     uint64_t interval_ms)
{
    if (!*last_ms || now_ms - *last_ms >= interval_ms) {
        *last_ms = now_ms;
        return true;
    }
    return false;
}

StateResult state_load(Emu *e, const char *savpath, const char *build_id,
                       char *why, size_t whysz);
int state_save(const Emu *e, const char *savpath, const char *build_id,
               char *why, size_t whysz);
int state_encode(const Emu *e, const char *build_id, uint8_t **blob, size_t *blob_len,
                 char *why, size_t whysz);
StateResult state_decode(Emu *e, const uint8_t *blob, size_t blob_len,
                         const char *build_id, char *why, size_t whysz);
int state_sav_lock_acquire(const char *savpath, uintptr_t *token,
                           char *why, size_t whysz);
void state_sav_lock_release(uintptr_t *token);

/* mem.c */
uint8_t  mem_read8 (Emu *e, uint32_t a);
uint16_t mem_read16(Emu *e, uint32_t a);
uint32_t mem_read32(Emu *e, uint32_t a);
void mem_write8 (Emu *e, uint32_t a, uint8_t v);
void mem_write16(Emu *e, uint32_t a, uint16_t v);
void mem_write32(Emu *e, uint32_t a, uint32_t v);

/* cpu.c */
void cpu_reset(Emu *e);
void cpu_step(Emu *e);         /* one instruction (ext prefixes folded in) */

static inline bool state_session_eligible(bool interactive, bool linkrom,
                                          bool net_host, bool net_join,
                                          bool net_peer, bool no_state)
{
    return interactive && !linkrom && !net_host && !net_join && !net_peer && !no_state;
}

/* Anything but a successful load starts from a cold reset. */
static inline StateResult state_restore_or_reset(Emu *e, const char *savpath,
                                                  const char *build_id,
                                                  bool eligible, bool restart,
                                                  char *why, size_t whysz)
{
    StateResult result = STATE_NONE;
    if (eligible && !restart)
        result = state_load(e, savpath, build_id, why, whysz);
    if (result != STATE_LOADED)
        cpu_reset(e);
    return result;
}

/* Don't snapshot a stopped core. Zero means skipped or failed, state_save supplies the error text when it failed. */
static inline int state_save_on_exit(const Emu *e, const char *savpath,
                                     const char *build_id, bool eligible,
                                     char *why, size_t whysz)
{
    return eligible && e && !e->stopped &&
           state_save(e, savpath, build_id, why, whysz);
}

/* disasm.c */
int disasm_one(Emu *e, uint32_t pc, char *out, size_t outsz); /* returns length in bytes consumed */

/* periph.c */
uint8_t  periph_read8(Emu *e, uint32_t a);
void periph_write8(Emu *e, uint32_t a, uint8_t v);
void periph_write32(Emu *e, uint32_t a, uint32_t v);
uint32_t periph_read32(Emu *e, uint32_t a);
void periph_tick(Emu *e, uint32_t cycles);
void periph_buttons(Emu *e, uint8_t mask);  /* update pressed-button mask (bit0-2 = A/B/C) */
int periph_speed_step(int cur, int dir);  /* game-clock ladder: next stop up (+) or down (-) */

/* pn512.c */
void pn512_power_on(Emu *e);                /* cold power-up, from cpu_reset */
void pn512_reset(Emu *e);                   /* register file to power-up values */
void pn512_host_byte(Emu *e, uint8_t b);    /* the fw wrote B10: one wire byte */
void pn512_rf_deliver(Emu *e, const uint8_t *frame, int len); /* a peer's frame */
/* add_crc 0 for the ISO14443-A activation answers, which carry a BCC or
 * nothing rather than the air CRC */
void pn512_rf_deliver_ex(Emu *e, const uint8_t *frame, int len, int add_crc);
void periph_nfc_field_edge(Emu *e);  /* peer RF at the antenna: latch IRQ-pin wake flag */
void pn512_probe_set(Emu *e, int on); /* the N key: store point into/out of the field */

/* --nfc-inject: the phone side of an item download (src/nfcpeer.c) */
int  nfcpeer_open(Emu *e, const char *payload_path);  /* 0 = failed, message printed */
void nfcpeer_rf_in(Emu *e, const uint8_t *frame, int len);  /* the 4U transmitted */
void nfcpeer_tick(Emu *e);                            /* deliver a due reply */
void nfcpeer_report(Emu *e, FILE *f);
void pn512_ca_tick(Emu *e);                 /* launch a CA-held frame when due */
void pn512_report(Emu *e, FILE *f);         /* session counters */

/* lcd.c */
void lcd_cmd(Emu *e, uint8_t c);
void lcd_data(Emu *e, uint8_t d);
uint8_t lcd_read(Emu *e, uint32_t a);
void lcd_render(Emu *e, uint32_t *out, int *w, int *h); /* RGBA8888, MADCTL applied */
int lcd_dump_bmp(Emu *e, const char *path);
void lcd_report(Emu *e, FILE *f);  /* gamma/power MATCH table vs iD expected */

void mem_io_log_touch(Emu *e, uint32_t a, int write, uint32_t v, int size);

/* link.c */
void link_reset(Link *l);
uint64_t link_now_us(void);                   /* host monotonic us; shared clock */
void link_set_tx_byte_us(Link *l, unsigned us); /* wire time of the byte being sent */
uint64_t link_collisions(Link *l);            /* bytes corrupted by overlap */
unsigned link_backlog_max(Link *l);           /* deepest rx FIFO backlog, bytes */
int      link_pend_max(Link *l);              /* deepest settle queue, records */
uint64_t link_send_fails(Link *l);            /* records the socket refused, i.e. lost */
uint64_t link_reorder_late(Link *l);          /* reorder-horizon tripwire count */
/* Test hooks inject timestamped records and control the release clock. */
void link_test_inject(Link *l, uint32_t id, uint8_t byte, uint64_t t_us, unsigned dur_us);
void link_test_release(Link *l);
void link_test_now(Link *l, uint64_t now_us); /* script the release clock */
int64_t link_test_offset(Link *l, uint32_t id); /* gap estimate; INT64_MAX if none */
void link_tx(Link *l, int from_core, uint8_t byte);  /* from_core: 0=A, 1=B */
/* Browser callers supply a shared release clock. Desktop wrappers use
 * link_now_us(). */
int  link_rx_at(Link *l, int for_core, uint64_t now_us);
int  link_rx_pending_at(Link *l, int for_core, uint64_t now_us);
int  link_rx(Link *l, int for_core);                 /* next byte, or -1 if none */
int  link_rx_pending(Link *l, int for_core);         /* 1 if a byte waits */

void link_core_step(Emu *e, uint8_t btn_mask);                 /* one instr + periodic tick/buttons */
void link_lockstep_run(Emu *a, Emu *b, uint64_t quantum,
                  uint8_t amask, uint8_t bmask);           /* advance both ~quantum cycles */
int link_route_key(int focus, int sdl_key_bit);   /* focused core for a button, else -1 */
void periph_ir_session_begin(Emu *e); /* session 0->nonzero: hold RX off for one byte-time */
uint8_t periph_ir_session_state(Emu *e); /* nonzero while the fw is inside an IR session */
void link_ir_pc_sample(Emu *e);  /* --ir-log: sample pc while IR is active */
void link_ir_pc_report(Emu *e);   /* --ir-log: dump where a core spent its time */
int  link_host(Link *l, int port);            /* --host: listen + accept one peer */
int  link_join(Link *l, const char *hostport);/* --join host:port */
int  link_peer(Link *l, int port);            /* --peer: join if present, else host */
int  link_auto_begin(Link *l, int port);      /* join, else start a non-blocking listener */
int  link_auto_poll(Link *l);                 /* 1 when a waiting peer has been accepted */
void link_close(Link *l);

/* Optional symbols used by disassembly and traces. */
int         disasm_syms_load(const char *path);   /* returns symbols loaded, 0 on failure */
const char *disasm_sym_for(uint32_t pc);          /* "NAME+0x12", or "" if unknown */

/* panel.c */
struct SDL_Renderer;
int  panel_hit(int x, int y);   /* window px -> button bit (1=A 2=B 4=C), 0 = nothing */
int  panel_keys_parse(const char *s, char out[3]);  /* three a-z/1-9 keys; s/n reserved */
void panel_draw(struct SDL_Renderer *ren, uint8_t pressed);  /* strip; lit circles = pressed */

#endif
