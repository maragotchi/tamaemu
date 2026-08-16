#include "../src/emu.h"
#include "../tools/dlc.h"
#include "../tools/swapreq.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* Install the profile before touching memory; a zeroed profile maps nothing. */
static Emu *fresh_dev(const DeviceProfile *dev)
{
    static Emu e;
    memset(&e, 0, sizeof e);
    e.dev = *dev;
    static uint8_t rom[PS_ROM_SIZE];
    memset(rom, 0xFF, sizeof rom);
    e.rom = rom;
    e.cmu.osc3_hz = 20e6;
    return &e;
}

static Emu *fresh(void)
{
    return fresh_dev(device_default());
}

static void load_prog(Emu *e, uint32_t addr, const uint16_t *hws, int n)
{
    for (int i = 0; i < n; i++) mem_write16(e, addr + (uint32_t)i * 2, hws[i]);
    e->pc = addr;
    e->sp = 0x7F00;
}

static void test_mem(void)
{
    Emu *e = fresh();
    mem_write32(e, 0x1000, 0xDEADBEEF);
    CHECK(mem_read32(e, 0x1000) == 0xDEADBEEF, "ram roundtrip");
    CHECK(mem_read8(e, 0x1000) == 0xEF, "ram little endian");
    CHECK(mem_read8(e, 0x00500000) == 0, "unmapped reads 0");
    mem_write32(e, 0x80000, 0xCAFEF00D);
    CHECK(mem_read32(e, 0x80000) == 0xCAFEF00D, "ivram roundtrip");
}

static void test_cpu_alu(void)
{
    Emu *e = fresh();
    const uint16_t prog[] = {
        0x6C50,          /* ld.w %r0,5   (cls3 op1=3 imm6=5 rd=0) */
        0x6030,          /* add %r0,3 */
        0x6880,          /* cmp %r0,8 */
    };
    load_prog(e, 0x1000, prog, 3);
    cpu_step(e); CHECK(e->r[0] == 5, "ld.w imm6: r0=%x", e->r[0]);
    cpu_step(e); CHECK(e->r[0] == 8, "add imm6: r0=%x", e->r[0]);
    CHECK(!e->f_z && !e->f_n && !e->f_c && !e->f_v, "add flags");
    cpu_step(e); CHECK(e->f_z && !e->f_c && !e->f_n, "cmp equal flags z=%d c=%d", e->f_z, e->f_c);

    /* borrow */
    const uint16_t prog2[] = { 0x6C01, /* ld.w %r1,0 */ 0x6411 /* sub %r1,1 */ };
    load_prog(e, 0x1100, prog2, 2);
    cpu_step(e); cpu_step(e);
    CHECK(e->r[1] == 0xFFFFFFFF && e->f_c && e->f_n && !e->f_z, "sub borrow c=%d n=%d", e->f_c, e->f_n);
}

static void test_cpu_ext(void)
{
    Emu *e = fresh();
    /* ext 0x1 ; ld.w %r2,0x12 -> imm=(1<<6)|0x12=0x52 */
    const uint16_t prog[] = { 0xC001, 0x6D22 };
    load_prog(e, 0x1000, prog, 2);
    cpu_step(e);
    CHECK(e->r[2] == 0x52, "xld.w single ext: r2=%x", e->r[2]);

    /* two ext: 0x1000,0x0000 ; ld.w %r3,0 -> 0x1000<<19 = 0x80000000 */
    const uint16_t prog2[] = { 0xD000, 0xC000, 0x6C03 };
    load_prog(e, 0x1100, prog2, 3);
    cpu_step(e);
    CHECK(e->r[3] == 0x80000000, "xld.w double ext: r3=%x", e->r[3]);

    /* signed single-ext cmp: ext 0x1FFF ; cmp %r0,0x3F -> imm = sext19(0x7FFFF) = -1 */
    e->r[0] = 0xFFFFFFFF;
    const uint16_t prog3[] = { 0xDFFF, 0x6BF0 };
    load_prog(e, 0x1200, prog3, 2);
    cpu_step(e);
    CHECK(e->f_z, "xcmp signed ext: z=%d (r0=-1 vs -1)", e->f_z);

    /* xadd 3-operand: ext 0x100 ; add %r5,%r6 (cls1 op2=2) -> r5 = r6 + 0x100 */
    e->r[6] = 7;
    const uint16_t prog4[] = { 0xC100, 0x2265 };
    load_prog(e, 0x1300, prog4, 2);
    cpu_step(e);
    CHECK(e->r[5] == 0x107, "xadd rd,rs,imm13: r5=%x", e->r[5]);
}

static void test_cpu_xjp_patch_bytes(void)
{
    /* Real contrast-patch bytes. Target = 0x2004 + (0x49 << 9 | 0xD8 << 1). */
    Emu *e = fresh();
    const uint16_t prog[] = { 0xC000, 0xC049, 0x1ED8 };
    load_prog(e, 0x2000, prog, 3);
    cpu_step(e);
    CHECK(e->pc == 0x2004 + 0x93B0, "xjp contrast-patch target: pc=%x want %x",
          e->pc, 0x2004 + 0x93B0);

    /* PLL patch xjp target = hwaddr + (0xC1 << 9 | 0x87 << 1). */
    const uint16_t prog2[] = { 0xC000, 0xC0C1, 0x1E87 };
    load_prog(e, 0x2100, prog2, 3);
    cpu_step(e);
    CHECK(e->pc == 0x2104 + ((0xC1 << 9) | (0x87 << 1)), "xjp PLL-patch target: pc=%x", e->pc);
}

static void test_cpu_branch_call(void)
{
    Emu *e = fresh();
    /* call +4 then check stack; hw: cls0 op1=14 d=0 disp8=2 -> 0x1C02 */
    const uint16_t prog[] = { 0x1C02, 0x0000, 0x6C10 /* target: ld.w %r0,1 */ };
    load_prog(e, 0x1000, prog, 3);
    uint32_t sp0 = e->sp;
    cpu_step(e);
    CHECK(e->pc == 0x1004, "call rel8 target pc=%x", e->pc);
    CHECK(e->sp == sp0 - 4 && mem_read32(e, e->sp) == 0x1002, "call pushed ret=%x",
          mem_read32(e, e->sp));

    /* jp.d +6 with slot add %r0,1: slot executes, then jump.
       jp.d hw: cls0 op1=15 d=1 disp8=3 -> (15<<9)|(1<<8)|3 = 0x1F03 */
    e->r[0] = 0;
    const uint16_t prog2[] = { 0x1F03, 0x6010 /* add %r0,1 */, 0x0000, 0x6C20 };
    load_prog(e, 0x1100, prog2, 4);
    cpu_step(e);
    CHECK(e->r[0] == 1, "delay slot executed: r0=%x", e->r[0]);
    CHECK(e->pc == 0x1106, "jp.d target: pc=%x", e->pc);

    /* pushn/popn r3 */
    e->r[0] = 10; e->r[1] = 11; e->r[2] = 12; e->r[3] = 13;
    const uint16_t prog3[] = { 0x0203 /* pushn %r3 */ };
    load_prog(e, 0x1200, prog3, 1);
    uint32_t sp1 = e->sp;
    cpu_step(e);
    CHECK(e->sp == sp1 - 16, "pushn depth");
    CHECK(mem_read32(e, e->sp) == 10 && mem_read32(e, e->sp + 12) == 13, "pushn order (r0 last)");
    e->r[0] = e->r[1] = e->r[2] = e->r[3] = 0;
    const uint16_t prog4[] = { 0x0243 /* popn %r3 */ };
    load_prog(e, 0x1210, prog4, 1);
    e->sp = sp1 - 16;
    cpu_step(e);
    CHECK(e->r[0] == 10 && e->r[3] == 13, "popn restores r0=%d r3=%d", e->r[0], e->r[3]);
}

static void test_cmu(void)
{
    Emu *e = fresh();
    /* protected write blocked */
    periph_write32(e, 0x301B08, 0x00770003);
    CHECK(e->cmu.sccr == 0, "protected write blocked");
    /* unlock, then the real reset-handler value: OSC3 direct */
    periph_write32(e, 0x301B24, 0x96);
    periph_write32(e, 0x301B08, 0x00770003);
    CHECK(fabs(e->cmu.mclk_hz - 20e6) < 1, "MCLK OSC3 direct = %f", e->cmu.mclk_hz);
    /* PLL A on iD crystal: D=6 (field 5), N=13 (field 12), M=2 (MCLKDIV=1), OSCSEL=PLL */
    e->cmu.osc3_hz = 18.432e6;
    periph_write32(e, 0x301B0C, (12u << 4) | 1u);
    periph_write32(e, 0x301B08, (5u << 20) | (1u << 12) | (3u << 2));
    CHECK(fabs(e->cmu.mclk_hz - 19.968e6) < 1e3, "MCLK PLL A = %f", e->cmu.mclk_hz);
    /* PLL B: D=3 (field 2), N=7 (field 6), M=2 */
    periph_write32(e, 0x301B0C, (6u << 4) | 1u);
    periph_write32(e, 0x301B08, (2u << 20) | (1u << 12) | (3u << 2));
    CHECK(fabs(e->cmu.mclk_hz - 21.504e6) < 1e3, "MCLK PLL B = %f", e->cmu.mclk_hz);
}

static void test_rtc_buttons(void)
{
    Emu *e = fresh();
    e->cmu.mclk_hz = 20e6;
    /* RTC: run (STP=0), count 61 emulated seconds -> 1 min 1 sec BCD */
    periph_write8(e, 0x301908, 0x10);   /* 24H, running */
    periph_write8(e, 0x301910, 0x00);
    periph_write8(e, 0x301914, 0x00);
    for (int i = 0; i < 61 * 20; i++) periph_tick(e, 1000000);  /* 61 s at 20 MHz */
    CHECK(periph_read8(e, 0x301910) == 0x01 && periph_read8(e, 0x301914) == 0x01,
          "rtc 61s -> %02x:%02x", periph_read8(e, 0x301914), periph_read8(e, 0x301910));
    /* BCD 9->10 carry */
    periph_write8(e, 0x301910, 0x09);
    for (int i = 0; i < 20; i++) periph_tick(e, 1000000);
    CHECK(periph_read8(e, 0x301910) == 0x10, "bcd carry sec=%02x", periph_read8(e, 0x301910));

    /* Pressing A pulls P00 low and raises its enabled ITC flag. */
    periph_write8(e, 0x300270, 0x07);
    periph_write8(e, 0x300260, 0x33);
    periph_buttons(e, 1);
    CHECK((periph_read8(e, 0x300380) & 1) == 0, "P00 low while pressed");
    CHECK(e->wake_req && e->pending_irq == 16, "port int 0 pending: vec=%u", e->pending_irq);
    periph_buttons(e, 0);
    CHECK((periph_read8(e, 0x300380) & 1) == 1, "P00 high released");
}

static void test_link(void)
{
    Link l;
    link_reset(&l);
    CHECK(link_rx(&l, 0) == -1 && link_rx(&l, 1) == -1, "empty link reads -1");
    CHECK(!link_rx_pending(&l, 0) && !link_rx_pending(&l, 1), "empty link no pending");

    link_tx(&l, 0, 0x5A);
    CHECK(link_rx_pending(&l, 1) && !link_rx_pending(&l, 0), "A->B pending for B only");
    CHECK(link_rx(&l, 1) == 0x5A, "B receives A's byte");
    CHECK(link_rx(&l, 1) == -1, "B drained");

    link_tx(&l, 1, 0x11); link_tx(&l, 1, 0x22);
    CHECK(link_rx(&l, 0) == 0x11 && link_rx(&l, 0) == 0x22, "A receives B in FIFO order");

    link_tx(&l, 0, 0xAA); link_tx(&l, 1, 0xBB);
    CHECK(link_rx(&l, 1) == 0xAA && link_rx(&l, 0) == 0xBB, "directions independent");

    /* wraparound past LINK_FIFO_N preserves order */
    for (unsigned i = 0; i < LINK_FIFO_N + 50; i++) {
        link_tx(&l, 0, (uint8_t)i);
        CHECK(link_rx(&l, 1) == (uint8_t)i, "wraparound order at %u", i);
    }
    CHECK(l.overflows == 0, "no overflow when drained each step");
    CHECK(l.bytes_ab > 0 && l.bytes_ba > 0, "lifetime counters move");

    /* A full FIFO drops new bytes and counts them. */
    {
        Link o; link_reset(&o);
        for (unsigned i = 0; i < LINK_FIFO_N + 10; i++) link_tx(&o, 0, (uint8_t)i);
        CHECK(o.overflows == 10, "overflow drops the 10 newest: %u", o.overflows);
        CHECK(link_rx_pending(&o, 1), "queue still has the first LINK_FIFO_N bytes");
        int ok = 1;
        for (unsigned i = 0; i < LINK_FIFO_N; i++)
            if (link_rx(&o, 1) != (int)(uint8_t)i) { ok = 0; break; }
        CHECK(ok, "kept bytes are the oldest LINK_FIFO_N in order (newest dropped)");
        CHECK(link_rx(&o, 1) == -1, "nothing beyond the kept LINK_FIFO_N");
    }

    /* A shared bus delivers to every participant except the sender. */
    {
        Link b; link_reset(&b);
        b.npeers = 3;
        link_tx(&b, 0, 0x77);
        CHECK(!link_rx_pending(&b, 0), "sender does not hear itself");
        CHECK(link_rx(&b, 1) == 0x77 && link_rx(&b, 2) == 0x77,
              "both other participants hear it");

        link_tx(&b, 2, 0x88);
        CHECK(link_rx(&b, 0) == 0x88 && link_rx(&b, 1) == 0x88, "fan-out from peer 2");
        CHECK(link_rx(&b, 2) == -1, "peer 2 still does not hear itself");

        /* Draining one participant must not drain another. */
        link_tx(&b, 0, 0x99);
        CHECK(link_rx(&b, 1) == 0x99, "peer 1 drains its own copy");
        CHECK(link_rx_pending(&b, 2), "peer 2's copy untouched by peer 1's read");
        CHECK(link_rx(&b, 2) == 0x99, "peer 2 gets its own copy");
    }

    /* An unset npeers still means a two-party link. */
    {
        Link z; link_reset(&z); z.npeers = 0;
        link_tx(&z, 0, 0x5A);
        CHECK(link_rx(&z, 1) == 0x5A, "npeers 0 still delivers A->B");
        CHECK(link_rx(&z, 2) == -1, "npeers 0 does not fan out to a third");
    }
}

/* Poll boundedly for loopback delivery, pumping the hub when it must relay. */
static int link_rx_wait(Link *l, Link *hub)
{
    for (int i = 0; i < 200000; i++) {
        if (hub) link_rx_pending(hub, 0);        /* drive the relay */
        int b = link_rx(l, 0);
        if (b >= 0) return b;
    }
    return -1;
}

/* Exercise guest-to-guest relay over real non-blocking loopback sockets. */
static void test_link_hub_relay(void)
{
    const int port = 47881;              /* high and unlikely to clash */
    static Link hub, g1, g2;
    link_reset(&hub); link_reset(&g1); link_reset(&g2);

    /* Use negligible wire time so this test isolates routing from collisions. */
    link_set_tx_byte_us(&hub, 1); link_set_tx_byte_us(&g1, 1); link_set_tx_byte_us(&g2, 1);

    if (link_auto_begin(&hub, port) || !hub.listening) {
        fprintf(stderr, "[test] port %d unavailable - skipping hub relay test\n", port);
        return;                          /* skip, do not fail: not the code's fault */
    }
    CHECK(hub.hub == 1, "auto_begin makes us the hub");

    char hp[64]; snprintf(hp, sizeof hp, "127.0.0.1:%d", port);
    CHECK(link_join(&g1, hp), "guest 1 connects");
    CHECK(link_auto_poll(&hub), "hub accepts guest 1");
    CHECK(hub.listening, "listener stays open after the first guest");

    CHECK(link_join(&g2, hp), "guest 2 connects to the same hub");
    CHECK(link_auto_poll(&hub), "hub accepts guest 2");
    CHECK(hub.nsock == 2, "hub holds both guests: %d", hub.nsock);

    /* The hub sends directly to every guest. */
    link_tx(&hub, 0, 0xA1);
    CHECK(link_rx_wait(&g1, NULL) == 0xA1, "guest 1 hears the hub");
    CHECK(link_rx_wait(&g2, NULL) == 0xA1, "guest 2 hears the hub");

    /* Guest traffic reaches the hub and is relayed to other guests. */
    link_tx(&g1, 0, 0xB2);
    CHECK(link_rx_wait(&hub, NULL) == 0xB2, "hub hears guest 1");
    CHECK(link_rx_wait(&g2, &hub) == 0xB2, "guest 2 hears guest 1 via the relay");
    CHECK(link_rx(&g1, 0) == -1, "guest 1 is not echoed its own byte");

    link_tx(&g2, 0, 0xC3);
    CHECK(link_rx_wait(&hub, NULL) == 0xC3, "hub hears guest 2");
    CHECK(link_rx_wait(&g1, &hub) == 0xC3, "guest 1 hears guest 2 via the relay");
    CHECK(link_rx(&g2, 0) == -1, "guest 2 is not echoed its own byte");

    for (int i = 0; i < 32; i++) link_tx(&g1, 0, (uint8_t)i);
    int ok = 1, got = -1;
    for (int i = 0; i < 32; i++)
        if ((got = link_rx_wait(&g2, &hub)) != i) { ok = 0; break; }
    CHECK(ok, "relayed bytes keep their order (got %d)", got);

    /* Drain the hub's copy before testing the next transmission. */
    while (link_rx(&hub, 0) >= 0) { }

    /* Exercise all LINK_PEERS_MAX participants. */
    static Link g3;
    link_reset(&g3);
    link_set_tx_byte_us(&g3, 1);
    CHECK(link_join(&g3, hp), "guest 3 connects");
    CHECK(link_auto_poll(&hub), "hub accepts guest 3");
    CHECK(hub.nsock == 3, "hub holds three guests: %d", hub.nsock);

    link_tx(&g3, 0, 0xD4);
    CHECK(link_rx_wait(&hub, NULL) == 0xD4, "hub hears guest 3");
    CHECK(link_rx_wait(&g1, &hub) == 0xD4, "guest 1 hears guest 3 via the relay");
    CHECK(link_rx_wait(&g2, &hub) == 0xD4, "guest 2 hears guest 3 via the relay");
    CHECK(link_rx(&g3, 0) == -1, "guest 3 is not echoed its own byte");

    link_tx(&hub, 0, 0xE5);
    CHECK(link_rx_wait(&g1, NULL) == 0xE5, "guest 1 hears the hub (4-way)");
    CHECK(link_rx_wait(&g2, NULL) == 0xE5, "guest 2 hears the hub (4-way)");
    CHECK(link_rx_wait(&g3, NULL) == 0xE5, "guest 3 hears the hub (4-way)");

    /* Reported counters must reflect real socket traffic. */
    CHECK(link_pend_max(&hub) > 0, "the settle-queue instrument fires on socket "
          "traffic: %d", link_pend_max(&hub));
    CHECK(link_backlog_max(&hub) > 0, "the rx-backlog instrument fires: %u",
          link_backlog_max(&hub));
    CHECK(link_send_fails(&hub) == 0 && link_send_fails(&g1) == 0,
          "loopback at this volume refuses nothing: hub %llu g1 %llu",
          (unsigned long long)link_send_fails(&hub),
          (unsigned long long)link_send_fails(&g1));

    link_close(&g1); link_close(&g2); link_close(&g3); link_close(&hub);
}

/* Explicit timestamps test collision policy without depending on scheduling. */
static void test_link_collision(void)
{
    const uint32_t A = 0xAAAA1111u, B = 0xBBBB2222u;
    const unsigned DUR = 100;                 /* 100us on the wire */

    /* disjoint in time: both survive untouched */
    {
        static Link l; link_reset(&l);
        link_test_inject(&l, A, 0x11, 1000, DUR);
        link_test_inject(&l, B, 0x22, 2000, DUR);   /* starts after A finished */
        link_test_release(&l);
        CHECK(link_collisions(&l) == 0, "back-to-back is not a collision: %llu",
              (unsigned long long)link_collisions(&l));
        CHECK(link_rx(&l, 0) == 0x11 && link_rx(&l, 0) == 0x22, "both bytes intact");
    }

    /* overlapping: neither byte may be delivered as sent. Two signals on top of
     * each other destroy one another; there is no "first one wins". */
    {
        static Link l; link_reset(&l);
        link_test_inject(&l, A, 0x11, 1000, DUR);
        link_test_inject(&l, B, 0x22, 1050, DUR);   /* starts mid-byte */
        link_test_release(&l);
        CHECK(link_collisions(&l) == 2, "both bytes are corrupted, not just one: %llu",
              (unsigned long long)link_collisions(&l));
        int b0 = link_rx(&l, 0), b1 = link_rx(&l, 0);
        CHECK(b0 != 0x11 && b0 >= 0, "first byte destroyed too: %02x", b0);
        CHECK(b1 != 0x22 && b1 >= 0, "the colliding byte is not what was sent: %02x", b1);
    }

    /* one device transmitting back-to-back is never a collision with itself */
    {
        static Link l; link_reset(&l);
        link_test_inject(&l, A, 0x33, 1000, DUR);
        link_test_inject(&l, A, 0x44, 1010, DUR);   /* same id, overlapping */
        link_test_release(&l);
        CHECK(link_collisions(&l) == 0, "a device does not collide with itself");
        CHECK(link_rx(&l, 0) == 0x33 && link_rx(&l, 0) == 0x44, "own bytes intact");
    }

    /* TCP may reorder arrival; release records by timestamp. */
    {
        static Link l; link_reset(&l);
        link_test_inject(&l, A, 0x99, 5000, DUR);   /* injected first, sent later */
        link_test_inject(&l, B, 0x88, 1000, DUR);
        link_test_release(&l);
        CHECK(link_rx(&l, 0) == 0x88, "oldest transmission is delivered first");
        CHECK(link_rx(&l, 0) == 0x99, "then the newer one");
    }

    /* A third, nonoverlapping transmission remains intact. */
    {
        static Link l; link_reset(&l);
        link_test_inject(&l, A, 0x11, 1000, DUR);
        link_test_inject(&l, B, 0x22, 1050, DUR);        /* collides with A */
        link_test_inject(&l, 0xCCCC3333u, 0x33, 9000, DUR);
        link_test_release(&l);
        int b0 = link_rx(&l, 0), b1 = link_rx(&l, 0), b2 = link_rx(&l, 0);
        CHECK(b0 != 0x11, "both colliders are corrupted (first)");
        CHECK(b1 != 0x22, "both colliders are corrupted (second)");
        CHECK(b2 == 0x33, "the third device, alone on the wire, is unharmed");
    }
}

/* Script the release clock to test due-time and offset calculations exactly. */
static void test_link_wire_time(void)
{
    const uint32_t A = 0xAAAA1111u, B = 0xBBBB2222u;
    const unsigned DUR = 87;

    /* A record remains held until its exact due time. */
    {
        static Link l; link_reset(&l); l.net = 1;
        link_test_now(&l, 1000);                    /* arrival == t: offset 0 */
        link_test_inject(&l, A, 0x11, 1000, DUR);
        link_test_now(&l, 1000 + LINK_LEAD_US - 1);
        CHECK(link_rx(&l, 0) < 0, "held until due (one us early: nothing)");
        link_test_now(&l, 1000 + LINK_LEAD_US);
        CHECK(link_rx(&l, 0) == 0x11, "released at due");
    }

    /* Batched records recover their original wire spacing. */
    {
        static Link l; link_reset(&l); l.net = 1;
        link_test_now(&l, 1870);       /* both arrive as the second is sent */
        link_test_inject(&l, A, 0x11, 1000, DUR);
        link_test_inject(&l, A, 0x22, 1870, DUR);
        /* offset = min(870, 0) = 0, so dues are 2500 and 3370 */
        link_test_now(&l, 2500);
        CHECK(link_rx(&l, 0) == 0x11, "first record out at its due");
        CHECK(link_rx(&l, 0) < 0, "second still held: the wire's silence");
        link_test_now(&l, 3369);
        CHECK(link_rx(&l, 0) < 0, "still silence one us before its due");
        link_test_now(&l, 3370);
        CHECK(link_rx(&l, 0) == 0x22, "second record out at its own due");
    }

    /* A record already past due releases immediately. */
    {
        static Link l; link_reset(&l); l.net = 1;
        link_test_now(&l, 1000);
        link_test_inject(&l, A, 0x11, 1000, DUR);   /* seeds offset = 0 */
        link_test_now(&l, 1000 + LINK_LEAD_US);
        CHECK(link_rx(&l, 0) == 0x11, "seed record out");
        link_test_now(&l, 9000);                    /* transport stalled... */
        link_test_inject(&l, A, 0x22, 3000, DUR);   /* ...this one is long due */
        CHECK(link_rx(&l, 0) == 0x22, "late record delivers immediately");
    }

    /* The offset estimate converges, ignores one slow record, and recovers after a stall. */
    {
        static Link l; link_reset(&l); l.net = 1;
        link_test_now(&l, 1300);
        link_test_inject(&l, A, 1, 1000, DUR);          /* delayed 300 */
        CHECK(link_test_offset(&l, A) == 300, "first record seeds the estimate: "
              "%lld", (long long)link_test_offset(&l, A));
        link_test_now(&l, 2080);
        link_test_inject(&l, A, 2, 2000, DUR);          /* delayed 80: new min */
        CHECK(link_test_offset(&l, A) == 80, "a faster record lowers it: %lld",
              (long long)link_test_offset(&l, A));
        link_test_now(&l, 3200);
        link_test_inject(&l, A, 3, 3000, DUR);          /* delayed 200 */
        CHECK(link_test_offset(&l, A) <= 85, "a slower record does not raise it "
              "(min, not average): %lld", (long long)link_test_offset(&l, A));
        /* After ten quiet seconds, decay must release the stale minimum. */
        link_test_now(&l, 10003200);
        link_test_inject(&l, A, 4, 10002800, DUR);      /* delayed 400 */
        CHECK(link_test_offset(&l, A) == 400, "the estimate recovers after a "
              "stall: %lld", (long long)link_test_offset(&l, A));
    }

    /* Wire-time release still applies collision handling. */
    {
        static Link l; link_reset(&l); l.net = 1;
        link_test_now(&l, 1050);
        link_test_inject(&l, A, 0x11, 1000, DUR);
        link_test_inject(&l, B, 0x22, 1050, DUR);       /* overlaps A */
        link_test_now(&l, 1050 + 2 * LINK_LEAD_US);
        int b0 = link_rx(&l, 0), b1 = link_rx(&l, 0);
        CHECK(link_collisions(&l) == 2, "overlap ruled at wire-time release: %llu",
              (unsigned long long)link_collisions(&l));
        CHECK(b0 >= 0 && b0 != 0x11 && b1 >= 0 && b1 != 0x22,
              "both colliders corrupted: %02x %02x", b0, b1);
    }

    /* The reorder tripwire fires when an older record arrives after release. */
    {
        static Link l; link_reset(&l); l.net = 1;
        link_test_now(&l, 1000);
        link_test_inject(&l, A, 0x11, 1000, DUR);
        link_test_now(&l, 1000 + LINK_LEAD_US);
        CHECK(link_rx(&l, 0) == 0x11, "record released");
        CHECK(link_reorder_late(&l) == 0, "tripwire silent on ordinary traffic");
        link_test_inject(&l, B, 0x22, 900, DUR);   /* older than the release */
        CHECK(link_reorder_late(&l) == 1, "tripwire fires on a reorder miss: %llu",
              (unsigned long long)link_reorder_late(&l));
    }
}

/* Script the release clock so network timing tests ignore host scheduling. */
#define IRT_BYTE_CYC 1600      /* 18.432 MHz * 10 bits / 115200 baud = one byte */
#define IRT_BYTE_US  87        /* the same byte in host microseconds */

static Emu *ir_rx_ready(Link *l)
{
    Emu *e = fresh();
    e->cmu.mclk_hz = 18432000.0;                /* the P's clock */
    e->link = l;
    e->a0ram[0xDB3] = 2;                        /* the fw's own "IR session live" flag */
    periph_write8(e, 0x300B16, 4);              /* baud divisor 4 = 115200 */
    periph_write8(e, 0x300B15, 1);              /* arm the receiver */
    e->ir_rx_next_at = 0;                       /* the arming hold is not what is under test */
    return e;
}

/* Record each byte's arrival cycle while draining RX as firmware would. */
static int ir_rx_collect(Emu *e, Link *l, uint64_t base_us,
                         uint64_t *at, int n, uint64_t budget_cyc)
{
    int got = 0;
    uint64_t start = e->cycles, end = e->cycles + budget_cyc;
    while (e->cycles < end && got < n) {
        e->cycles += 64;
        link_test_now(l, base_us +
                      (uint64_t)((double)(e->cycles - start) * 1e6 / e->cmu.mclk_hz));
        periph_tick(e, 64);
        if (e->ioram[0x300B12 - e->dev.io_base] & 0x01u) {          /* IR_STAT rx-ready */
            at[got++] = e->cycles;
            e->ioram[0x300B12 - e->dev.io_base] &= (uint8_t)~0x01u; /* fw drained it */
        }
    }
    return got;
}

static void test_ir_rx_wire_time(void)
{
    /* Simultaneous UART senders share one link due time but enter the firmware
     * one byte-time apart. */
    {
        static Link l; link_reset(&l); l.net = 1;
        link_test_now(&l, 1000);                      /* arrival == t: offset 0 */
        link_test_inject(&l, 0xA1A1A1A1u, 0x11, 1000, IRT_BYTE_US);
        link_test_inject(&l, 0xB2B2B2B2u, 0x22, 1000, IRT_BYTE_US);
        link_test_inject(&l, 0xC3C3C3C3u, 0x33, 1000, IRT_BYTE_US);
        Emu *e = ir_rx_ready(&l);
        uint64_t at[3];
        int n = ir_rx_collect(e, &l, 1000, at, 3, 40 * IRT_BYTE_CYC);
        CHECK(n == 3, "three simultaneous transmissions all hand over: %d", n);
        CHECK(link_collisions(&l) == 3, "all three are corrupted: %llu",
              (unsigned long long)link_collisions(&l));
        if (n == 3) {
            double span = (double)(at[2] - at[0]) / IRT_BYTE_CYC;
            fprintf(stderr, "[measure] 3 devices transmitting at one instant take "
                    "%.1f byte-times of the receiver's timeline, first byte to last "
                    "(hardware: 0.0 - one superimposed byte)\n", span);
            CHECK(span > 1.5 && span < 3.0, "simultaneous transmissions hand over "
                  "one byte-time each, the deliberate dilation: %.1f byte-times", span);
        }
    }

    /* A sender's silence must survive release and receiver pacing. */
    {
        static Link l; link_reset(&l); l.net = 1;
        /* Deliver both records in one simulated TCP batch. */
        link_test_now(&l, 1000 + 10 * IRT_BYTE_US);
        link_test_inject(&l, 0xA1A1A1A1u, 0x11, 1000, IRT_BYTE_US);
        link_test_inject(&l, 0xA1A1A1A1u, 0x22, 1000 + 10 * IRT_BYTE_US, IRT_BYTE_US);
        Emu *e = ir_rx_ready(&l);
        uint64_t at[2];
        int n = ir_rx_collect(e, &l, 1000 + 10 * IRT_BYTE_US, at, 2, 60 * IRT_BYTE_CYC);
        CHECK(n == 2, "both bytes delivered: %d", n);
        CHECK(link_collisions(&l) == 0, "one device, no collision");
        if (n == 2) {
            double gap = (double)(at[1] - at[0]) / IRT_BYTE_CYC;
            fprintf(stderr, "[measure] a 10 byte-time gap on the wire reaches the fw "
                    "as %.1f byte-times\n", gap);
            CHECK(gap > 8.0 && gap < 12.0, "wire silence is reproduced, not collapsed "
                  "or stretched: %.1f byte-times", gap);
        }
    }

    /* The reported backlog must track the receive queue. */
    {
        static Link l; link_reset(&l); l.net = 1;
        for (int i = 0; i < 5; i++)
            link_test_inject(&l, 0xA1A1A1A1u, (uint8_t)i, 1000 + (uint64_t)i * IRT_BYTE_US,
                             IRT_BYTE_US);
        link_test_release(&l);
        CHECK(link_backlog_max(&l) == 5, "backlog high-water sees all five queued: %u",
              link_backlog_max(&l));
        CHECK(link_pend_max(&l) == 0, "the settle queue counter is a net-pump measure");
        CHECK(link_send_fails(&l) == 0, "no sockets, no refused records");
    }
}

static void load_loop(Emu *e, uint32_t addr)
{
    /* add %r0,1 (0x6010) ; jp -2  (cls0 op1=13 d=0 disp8=-1=0xFF -> 0x1AFF) */
    mem_write16(e, addr, 0x6010);
    mem_write16(e, addr + 2, 0x1AFF);
    e->pc = addr; e->sp = 0x7F00;
}

static void test_lockstep(void)
{
    static Emu a, b; static uint8_t roma[PS_ROM_SIZE], romb[PS_ROM_SIZE];
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    memset(roma, 0xFF, sizeof roma); memset(romb, 0xFF, sizeof romb);
    /* Both cores need profiles before execution. */
    a.dev = b.dev = *device_default();
    a.rom = roma; b.rom = romb;
    a.cmu.mclk_hz = b.cmu.mclk_hz = 20e6;
    load_loop(&a, 0x1000); load_loop(&b, 0x1000);

    for (int q = 0; q < 20; q++) {
        uint64_t a0 = a.cycles;
        link_lockstep_run(&a, &b, 1000, 0, 0);
        CHECK(a.cycles - a0 >= 1000, "core A advanced a full quantum: %llu",
              (unsigned long long)(a.cycles - a0));
        /* Clocks stay within one quantum plus one instruction. */
        uint64_t skew = a.cycles > b.cycles ? a.cycles - b.cycles : b.cycles - a.cycles;
        CHECK(skew <= 1001, "cores stay in lockstep, skew=%llu", (unsigned long long)skew);
    }
    CHECK(!a.stopped && !b.stopped, "both cores still running");
}

static void test_route_key(void)
{
    CHECK(link_route_key(0, 1) == 0, "A-press with focus A -> core 0");
    CHECK(link_route_key(1, 2) == 1, "B-press with focus B -> core 1");
    CHECK(link_route_key(0, 0) == -1, "non-button key -> no core");
    CHECK(link_route_key(1, 4) == 1, "C-press with focus B -> core 1");
}

/* Clock steps choose the nearest ladder value in the requested direction. */
static void test_speed_step(void)
{
    static const int ladder[] = {1, 2, 5, 10, 30, 60, 120, 300, 600};
    const int n = (int)(sizeof ladder / sizeof ladder[0]);
    for (int i = 0; i + 1 < n; i++) {
        CHECK(periph_speed_step(ladder[i], +1) == ladder[i + 1],
              "x%d steps up to x%d, got x%d",
              ladder[i], ladder[i + 1], periph_speed_step(ladder[i], +1));
        CHECK(periph_speed_step(ladder[i + 1], -1) == ladder[i],
              "x%d steps down to x%d, got x%d",
              ladder[i + 1], ladder[i], periph_speed_step(ladder[i + 1], -1));
    }
    CHECK(periph_speed_step(600, +1) == 600, "top clamps: %d", periph_speed_step(600, +1));
    CHECK(periph_speed_step(1, -1) == 1, "bottom clamps: %d", periph_speed_step(1, -1));

    /* An unset rtc_mult behaves as real time. */
    CHECK(periph_speed_step(0, +1) == 2, "unset clock steps up to x2: %d", periph_speed_step(0, +1));
    CHECK(periph_speed_step(0, -1) == 1, "unset clock steps down to x1: %d", periph_speed_step(0, -1));

    /* Off-ladder values snap in the requested direction. */
    CHECK(periph_speed_step(47, +1) == 60, "x47 snaps up to x60: %d", periph_speed_step(47, +1));
    CHECK(periph_speed_step(47, -1) == 30, "x47 snaps down to x30: %d", periph_speed_step(47, -1));

    /* Past the top of the ladder: stepping up must not walk backwards to 600. */
    CHECK(periph_speed_step(1000, +1) == 1000, "x1000 stays put going up: %d", periph_speed_step(1000, +1));
    CHECK(periph_speed_step(1000, -1) == 600, "x1000 steps down to x600: %d", periph_speed_step(1000, -1));
}

/* Pin the button layout with literal window coordinates, independent of panel.c. */
static void test_panel_hit(void)
{
    CHECK(panel_hit(64, 420) == 1, "centre of A -> bit 1: %d", panel_hit(64, 420));
    CHECK(panel_hit(192, 420) == 2, "centre of B -> bit 2: %d", panel_hit(192, 420));
    CHECK(panel_hit(320, 420) == 4, "centre of C -> bit 4: %d", panel_hit(320, 420));

    CHECK(panel_hit(128, 420) == 0, "gap A-B is dead: %d", panel_hit(128, 420));
    CHECK(panel_hit(256, 420) == 0, "gap B-C is dead: %d", panel_hit(256, 420));

    CHECK(panel_hit(0, 384) == 0, "strip top-left dead: %d", panel_hit(0, 384));
    CHECK(panel_hit(383, 384) == 0, "strip top-right dead: %d", panel_hit(383, 384));
    CHECK(panel_hit(0, 455) == 0, "strip bottom-left dead: %d", panel_hit(0, 455));
    CHECK(panel_hit(383, 455) == 0, "strip bottom-right dead: %d", panel_hit(383, 455));

    /* The circle boundary is inclusive. */
    CHECK(panel_hit(86, 420) == 1, "A right edge hits: %d", panel_hit(86, 420));
    CHECK(panel_hit(87, 420) == 0, "one past A's edge misses: %d", panel_hit(87, 420));
    CHECK(panel_hit(192, 398) == 2, "B top edge hits: %d", panel_hit(192, 398));
    CHECK(panel_hit(192, 397) == 0, "one above B's edge misses: %d", panel_hit(192, 397));

    /* Diagonal points distinguish the circular hit box from a square. */
    CHECK(panel_hit(79, 435) == 1, "inside A diagonally: %d", panel_hit(79, 435));
    CHECK(panel_hit(80, 436) == 0, "outside A diagonally: %d", panel_hit(80, 436));

    /* Clicks in the LCD area must never press a button. */
    CHECK(panel_hit(64, 383) == 0, "above A is screen, not button: %d", panel_hit(64, 383));
    CHECK(panel_hit(192, 0) == 0, "top of screen dead: %d", panel_hit(192, 0));
    CHECK(panel_hit(320, 383) == 0, "above C is screen, not button: %d", panel_hit(320, 383));
}

/* A wild jump must fault before zero-filled unmapped space becomes a NOP loop. */
static void test_cpu_bad_pc_trap(void)
{
    Emu *e = fresh();
    static const uint16_t jp_r0[] = { 0x0680 };      /* jp %r0 */
    load_prog(e, 0x1000, jp_r0, 1);
    e->r[0] = 0x2A542A30;                            /* unmapped target */
    cpu_step(e);
    CHECK(e->pc == 0x2A542A30, "jp took the wild target: pc=%08x", e->pc);
    CHECK(!e->stopped, "the jump itself does not fault");
    cpu_step(e);                                     /* the fetch there must trap */
    CHECK(e->stopped, "unmapped fetch stopped the core");
    CHECK(strstr(e->stop_reason, "unmapped") != NULL, "stop reason: %s", e->stop_reason);

    /* Legal execution must not trigger the trap. */
    e = fresh();                                     /* note: fresh() reuses one static Emu */
    static const uint16_t nops[] = { 0x0000, 0x0000 };
    load_prog(e, 0x1000, nops, 2);                   /* A0RAM: fw runs trampolines here */
    cpu_step(e); cpu_step(e);
    CHECK(!e->stopped, "A0RAM execution not trapped: %s", e->stop_reason);
    e->rom[0x100] = 0; e->rom[0x101] = 0;            /* fresh() fills ROM with 0xFF */
    e->pc = e->dev.rom_base + 0x100;
    cpu_step(e);
    CHECK(!e->stopped, "ROM execution not trapped: %s", e->stop_reason);
    e->pc = e->dev.ivram_base;
    cpu_step(e);
    CHECK(!e->stopped, "IVRAM execution not trapped: %s", e->stop_reason);
}

/* Exercise ranged NOR-write logging and its line budget. */
/* AMD unlock addresses are relative to the selected device's ROM base. */
static void unlock_program(Emu *e)
{
    mem_write16(e, e->dev.rom_base + 0xAAA, 0xAA);
    mem_write16(e, e->dev.rom_base + 0x554, 0x55);
    mem_write16(e, e->dev.rom_base + 0xAAA, 0xA0);
}

static void test_watch_flash(void)
{
    Emu *e = fresh();
    e->fwatch_lo = e->dev.rom_base + 0x100000; e->fwatch_hi = e->dev.rom_base + 0x10000F;
    e->fwatch_budget = 4;

    unlock_program(e);
    mem_write16(e, e->dev.rom_base + 0x100000, 0x0102);   /* in range -> consumes budget */
    CHECK(e->fwatch_budget == 3, "in-range program logged: budget=%d", e->fwatch_budget);
    CHECK(e->rom[0x100000] == 0x02, "program actually happened: %02x", e->rom[0x100000]);

    unlock_program(e);
    mem_write16(e, e->dev.rom_base + 0x101000, 0x0304);   /* out of range -> no log */
    CHECK(e->fwatch_budget == 3, "out-of-range program ignored: budget=%d", e->fwatch_budget);

    e->fwatch_budget = 0;                          /* exhausted -> stays put */
    unlock_program(e);
    mem_write16(e, e->dev.rom_base + 0x100002, 0x0506);
    CHECK(e->fwatch_budget == 0, "budget floor holds: %d", e->fwatch_budget);
    CHECK(e->rom[0x100002] == 0x06, "programming still works with budget spent: %02x",
          e->rom[0x100002]);

    e = fresh();                                   /* watch off by default */
    unlock_program(e);
    mem_write16(e, e->dev.rom_base + 0x100000, 0x0102);
    CHECK(e->fwatch_budget == 0 && e->rom[0x100000] == 0x02,
          "disabled watch leaves programming untouched: %02x", e->rom[0x100000]);
}

/* Test each memory map through its profile addresses. */

static void test_device_table(void)
{
    int n = 0;
    for (const DeviceProfile *d; (d = device_at((size_t)n)) != NULL; n++) {
        CHECK(device_check(d, stderr), "%s: profile is self-consistent", d->name);
        CHECK(d->name && d->title, "device %d is named", n);
        CHECK(device_find(d->name) == d, "%s: findable by name", d->name);
        /* TTBR must point inside this device's NOR. */
        CHECK(d->ttbr_reset - d->rom_base < d->rom_size,
              "%s: ttbr_reset %08x lies inside ROM %08x+%x",
              d->name, d->ttbr_reset, d->rom_base, d->rom_size);
        for (int j = 0; j < n; j++)
            CHECK(strcmp(device_at((size_t)j)->name, d->name) != 0,
                  "%s: name is unique", d->name);
    }
    CHECK(n >= 2, "the table holds more than one machine (%d) - with one, nothing "
                  "shows whether the profile is read or ignored", n);
    CHECK(device_find("no-such-device") == NULL, "unknown device is rejected");
    CHECK(!strcmp(device_default()->name, "ps"), "the default machine is the P's");
}

/* Session state may be absolute or reached through a profile context pointer. */
static void test_ir_mode(void)
{
    const DeviceProfile *ps = device_find("ps"), *idl = device_find("idl");
    CHECK(ps && idl, "both machines with a measured IR session flag are in the table");
    if (!ps || !idl) return;

    CHECK(ps->ir_mode_flag && !ps->ir_ctx_ptr, "ps: flag form, not context form");
    CHECK(idl->ir_ctx_ptr && !idl->ir_mode_flag, "idl: context form, not flag form");
    /* iD L must receive peer traffic before entering a session. */
    CHECK(idl->ir_mode_flag == 0,
          "idl: the receive path is not gated on a session it has not entered yet");
    /* iD and Melody use different context-pointer addresses. */
    {
        const DeviceProfile *id = device_find("id"), *mel = device_find("id-melody");
        CHECK(id && mel, "both iD builds are in the table");
        if (id && mel) {
            CHECK(id->ir_ctx_ptr == 0x000842BCu && !id->ir_mode_flag,
                  "id: context form at 0x842BC, no receive gate");
            CHECK(mel->ir_ctx_ptr == 0x000842B8u && !mel->ir_mode_flag,
                  "id-melody: context form at 0x842B8, no receive gate");
            CHECK(id->ir_ctx_ptr != mel->ir_ctx_ptr,
                  "the two iD builds keep their IR context at different addresses");
        }
    }

    Emu *e = fresh_dev(ps);
    CHECK(periph_ir_session_state(e) == 0, "ps: cleared RAM is not in a session");
    mem_write8(e, ps->ir_mode_flag, 2);
    CHECK(periph_ir_session_state(e) == 2, "ps: reads the visit/connect value through the flag");
    mem_write8(e, ps->ir_mode_flag, 0);
    CHECK(periph_ir_session_state(e) == 0, "ps: session end clears it");

    e = fresh_dev(idl);
    CHECK(periph_ir_session_state(e) == 0, "idl: a null context pointer is not a session");
    /* A null context must not read a plausible byte at the offset address. */
    mem_write8(e, IR_CTX_STATE, 2);
    CHECK(periph_ir_session_state(e) == 0, "idl: a null context pointer is not followed");
    mem_write8(e, IR_CTX_STATE, 0);
    /* The test context lives at A0RAM 0x2000. */
    mem_write32(e, idl->ir_ctx_ptr, 0x2000);
    CHECK(periph_ir_session_state(e) == 0, "idl: a context whose state byte is 0 is not a session");
    mem_write8(e, 0x2000 + IR_CTX_STATE, 2);
    CHECK(periph_ir_session_state(e) == 2, "idl: reads the state byte through the context pointer");
    /* This nonzero value catches byte-swapped pointer reads. */
    mem_write8(e, 0x20 + IR_CTX_STATE, 7);
    CHECK(periph_ir_session_state(e) == 2, "idl: the context pointer is read little-endian");
    mem_write8(e, 0x2000 + IR_CTX_STATE, 0);
    CHECK(periph_ir_session_state(e) == 0, "idl: session end clears it");

    /* Sleep flags are profile-specific. */
    CHECK(ps->has_sleep_flag && ps->sleep_flag == 0x146C,
          "ps: the measured idle-sleep flag");
    CHECK(idl->has_sleep_flag && idl->sleep_flag == 0xFD8,
          "idl: the measured idle-sleep state byte");
    /* Hexagontchi uses the valid sleep-flag address zero. */
    {
        const DeviceProfile *pc = device_find("plus-color");
        const DeviceProfile *hx = device_find("plus-color-hexa");
        CHECK(pc && pc->has_sleep_flag && pc->sleep_flag == 0x368,
              "plus-color: the measured idle-sleep state byte");
        CHECK(hx && hx->has_sleep_flag && hx->sleep_flag == 0,
              "plus-color-hexa: state byte at 0x0000, expressible only via has_sleep_flag");
    }
    /* iD and Melody also use different sleep-flag addresses. */
    {
        const DeviceProfile *id = device_find("id");
        const DeviceProfile *im = device_find("id-melody");
        CHECK(id && id->has_sleep_flag && id->sleep_flag == 0xEFC,
              "id: the measured idle-sleep gate byte");
        CHECK(im && im->has_sleep_flag && im->sleep_flag == 0x11E0,
              "id-melody: the measured idle-sleep gate byte");
    }
    for (size_t i = 0; ; i++) {
        const DeviceProfile *d = device_at(i);
        if (!d) break;
        CHECK(!(d->sleep_flag && !d->has_sleep_flag),
              "%s: nonzero sleep_flag must carry has_sleep_flag", d->name);
        if (!d->has_sleep_flag) continue;
        CHECK(d->sleep_flag - d->a0ram_base < d->a0ram_size,
              "%s: sleep_flag %08x lies inside its own A0RAM", d->name, d->sleep_flag);
    }

    /* Missing session fields must not fall back to address zero. */
    DeviceProfile bare = *idl;
    bare.ir_ctx_ptr = 0;
    e = fresh_dev(&bare);
    mem_write8(e, IR_CTX_STATE, 2);
    CHECK(periph_ir_session_state(e) == 0, "an unexamined machine reports no session, not a stray byte");

    /* Reject a context pointer whose offset wraps into valid RAM. */
    e = fresh_dev(idl);
    mem_write8(e, (uint32_t)(0xFFFFFF00u + IR_CTX_STATE), 2);
    mem_write32(e, idl->ir_ctx_ptr, 0xFFFFFF00u);
    CHECK(periph_ir_session_state(e) == 0, "idl: a context pointer that wraps into RAM is not followed");
}

/* GPIO IR */
static void test_ir_gpio(void)
{
    const DeviceProfile *pc = device_find("plus-color");
    const DeviceProfile *hx = device_find("plus-color-hexa");
    CHECK(pc && hx, "both Plus Color builds are in the table");
    if (!pc || !hx) return;
    CHECK(pc->ir_gpio && pc->ir_gpio_tx_bit == 3 && pc->ir_gpio_rx_bit == 5,
          "plus-color: GPIO IR on the measured pins");
    CHECK(hx->ir_gpio && hx->ir_gpio_tx_bit == 3 && hx->ir_gpio_rx_bit == 5,
          "plus-color-hexa: same pins, measured on its own ROM");
    CHECK(!pc->ir_mode_flag && !pc->ir_ctx_ptr && !hx->ir_mode_flag && !hx->ir_ctx_ptr,
          "plus-color: polled RX has no session gate to declare");

    /* Polled receivers need a connect-driver PC range for auto-link detection. */
    CHECK(pc->ir_code_lo == 0xC31000u && pc->ir_code_hi == 0xC32400u,
          "plus-color: the measured connect-driver range");
    CHECK(hx->ir_code_lo == 0xC44300u && hx->ir_code_hi == 0xC45700u,
          "plus-color-hexa: the same driver at this build's +0x13300");

    /* The receive envelope idles high and goes low during a carrier mark. */
    Emu *e = fresh_dev(pc);
    CHECK(periph_read8(e, 0x300380) & 0x20, "plus-color: envelope idles high");
    e->irg_rx_marking = 1;
    CHECK(!(periph_read8(e, 0x300380) & 0x20), "plus-color: carrier reads low");
    e->irg_rx_marking = 0;
    /* Button inputs remain independent of the GPIO output latch. */
    periph_write8(e, 0x300380, 0x08);
    periph_write8(e, 0x300380, 0x00);
    CHECK((periph_read8(e, 0x300380) & 0x07) == 0x07,
          "plus-color: buttons unpressed read high whatever the latch holds");
}

/* PN512 host protocol */
static void nfc_wr(Emu *e, uint8_t reg, uint8_t val)
{
    periph_write8(e, 0x300B10, reg);
    periph_write8(e, 0x300B10, val);
}

static int nfc_reply(Emu *e)     /* pump once; return one reply or -1 */
{
    periph_tick(e, 1);
    if (!(periph_read8(e, 0x300B12) & 1)) return -1;
    return periph_read8(e, 0x300B11);
}

static int nfc_rd(Emu *e, uint8_t reg)
{
    periph_write8(e, 0x300B10, 0x80 | reg);
    return nfc_reply(e);
}

static void test_pn512(void)
{
    const DeviceProfile *d4 = device_find("4u"), *d4p = device_find("4u-plain");
    CHECK(d4 && d4->nfc_pn512, "the 4U+ carries the PN512");
    CHECK(d4p && d4p->nfc_pn512, "the plain 4U carries the same reader");
    CHECK(!device_find("ps")->nfc_pn512 && !device_find("idl")->nfc_pn512,
          "the IR machines do not");
    if (!d4) return;

    Emu *e = fresh_dev(d4);
    e->ir_fast = true;                    /* no baud pacing in the unit test */
    periph_write8(e, 0x300B15, 1);        /* receive strobe on */

    /* Register writes acknowledge by echoing the address. */
    nfc_wr(e, 0x00, 0x55);
    CHECK(nfc_reply(e) == 0x00, "write ack echoes the address byte");

    /* Wake preserves Idle while clearing power-down. */
    nfc_wr(e, 0x01, 0x27);
    CHECK(nfc_reply(e) == 0x01, "CommandReg write acks its address");
    int v = nfc_rd(e, 0x01);
    CHECK(v >= 0 && (v & 0x2F) == 0x20, "awake, RcvOff, Idle: %02x", v);

    /* RandomID and WrNFCIDtoFIFO produce ten bytes. */
    nfc_wr(e, 0x01, 0x22); nfc_reply(e);  /* RcvOff | RandomID */
    nfc_wr(e, 0x0A, 0x80); nfc_reply(e);  /* FlushBuffer */
    nfc_wr(e, 0x0C, 0x20); nfc_reply(e);  /* WrNFCIDtoFIFO */
    CHECK(nfc_rd(e, 0x0A) == 10, "the generated NFCID fills the FIFO");
    for (int i = 0; i < 10; i++) nfc_rd(e, 0x09);
    CHECK(nfc_rd(e, 0x0A) == 0, "reading the FIFO drains it");

    /* WUPA in an empty field sets TxIRq without receiving an answer. */
    nfc_wr(e, 0x14, 0x83); nfc_reply(e);
    nfc_wr(e, 0x0A, 0x80); nfc_reply(e);
    nfc_wr(e, 0x09, 0x52); nfc_reply(e);  /* WUPA */
    nfc_wr(e, 0x01, 0x0C); nfc_reply(e);  /* Transceive */
    nfc_wr(e, 0x0D, 0x87); nfc_reply(e);  /* StartSend, 7-bit frame */
    v = nfc_rd(e, 0x04);
    CHECK(v >= 0 && (v & 0x40), "TxIRq: the frame left the antenna: %02x", v);
    CHECK(nfc_rd(e, 0x0A) == 0, "an empty field answers nothing");
    v = nfc_rd(e, 0x08);
    CHECK(v >= 0 && (v & 7) != 2 && (v & 7) != 3,
          "modem waits for data rather than reporting mid-transmit: %02x", v);

    /* SoftReset returns the chip awake; PowerDown and wake toggle bit 4. */
    nfc_wr(e, 0x01, 0x2F); nfc_reply(e);  /* RcvOff | SoftReset */
    v = nfc_rd(e, 0x01);
    CHECK(v >= 0 && (v & 0x30) == 0x20, "after SoftReset: awake, RcvOff: %02x", v);
    nfc_wr(e, 0x01, 0x37); nfc_reply(e);  /* RcvOff | PowerDown | NoCmdChange */
    v = nfc_rd(e, 0x01);
    CHECK(v >= 0 && (v & 0x10), "PowerDown reads back while asleep: %02x", v);
    nfc_wr(e, 0x01, 0x27); nfc_reply(e);
    v = nfc_rd(e, 0x01);
    CHECK(v >= 0 && !(v & 0x10), "writing bit4 low wakes it: %02x", v);

    /* B15 low must hold replies during baud changes. */
    periph_write8(e, 0x300B15, 0);
    periph_write8(e, 0x300B10, 0x81);
    periph_tick(e, 1);
    CHECK(!(periph_read8(e, 0x300B12) & 1), "no delivery while the strobe is down");
    periph_write8(e, 0x300B15, 1);
    CHECK(nfc_reply(e) >= 0, "the held reply arrives once it rises");
}

/* Linked readers deliver RF frames to each other's FIFO. */
static void test_pn512_rf_link(void)
{
    const DeviceProfile *d4 = device_find("4u");
    if (!d4) return;
    /* fresh_dev reuses one static Emu, so keep the pair local. */
    static Emu a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.dev = b.dev = *d4;
    a.cmu.osc3_hz = b.cmu.osc3_hz = 30e6;
    b.core_id = 1;
    a.nfc_peer = &b; b.nfc_peer = &a;
    static uint8_t rom[PS_ROM_SIZE];
    a.rom = b.rom = rom;
    a.ir_fast = b.ir_fast = true;
    periph_write8(&a, 0x300B15, 1);
    periph_write8(&b, 0x300B15, 1);

    static const uint8_t atr_req[17] = {0x11, 0xD4, 0x00, 0xF0, 0x40, 0x31,
        0x18, 0xB0, 0x45, 0x46, 0x66, 0xF0, 0x40, 0x00, 0x00, 0x00, 0x00};

    /* With the field on, StartSend transmits immediately. */
    nfc_wr(&a, 0x14, 0x83); nfc_reply(&a);
    nfc_wr(&b, 0x14, 0x83); nfc_reply(&b);

    /* Frames disappear when no receiver is listening. */
    nfc_wr(&a, 0x0A, 0x80); nfc_reply(&a);
    for (int i = 0; i < 17; i++) { nfc_wr(&a, 0x09, atr_req[i]); nfc_reply(&a); }
    nfc_wr(&a, 0x01, 0x0C); nfc_reply(&a);
    nfc_wr(&a, 0x0D, 0x80); nfc_reply(&a);
    CHECK(nfc_rd(&b, 0x0A) == 0, "a frame nobody listened for is gone");
    CHECK(b.nfc.rf_dropped == 1, "and counted as dropped");

    /* B's Receive command accepts A's next frame and raises RxIRq. */
    nfc_wr(&b, 0x0A, 0x80); nfc_reply(&b);
    nfc_wr(&b, 0x01, 0x08); nfc_reply(&b);
    nfc_wr(&a, 0x0A, 0x80); nfc_reply(&a);
    for (int i = 0; i < 17; i++) { nfc_wr(&a, 0x09, atr_req[i]); nfc_reply(&a); }
    nfc_wr(&a, 0x01, 0x0C); nfc_reply(&a);
    nfc_wr(&a, 0x0D, 0x80); nfc_reply(&a);
    /* The 17-byte payload and two-byte air CRC fill 19 FIFO bytes. */
    CHECK(nfc_rd(&b, 0x0A) == 19, "the ATR_REQ lands whole, CRC included");
    int v = nfc_rd(&b, 0x04);
    CHECK(v >= 0 && (v & 0x20), "RxIRq set for the listener: %02x", v);
    for (int i = 0; i < 17; i++) {
        int rb = nfc_rd(&b, 0x09);
        CHECK(rb == atr_req[i], "frame byte %d: %02x want %02x", i, rb, atr_req[i]);
    }
    nfc_rd(&b, 0x09); nfc_rd(&b, 0x09);        /* drain the CRC */

    /* Transceive leaves A armed for B's reply. */
    nfc_wr(&b, 0x0A, 0x80); nfc_reply(&b);
    nfc_wr(&b, 0x09, 0x03); nfc_reply(&b);
    nfc_wr(&b, 0x09, 0xD5); nfc_reply(&b);
    nfc_wr(&b, 0x09, 0x01); nfc_reply(&b);
    nfc_wr(&b, 0x01, 0x0C); nfc_reply(&b);
    nfc_wr(&b, 0x0D, 0x80); nfc_reply(&b);
    CHECK(nfc_rd(&a, 0x0A) == 5, "the reply crosses back into A's FIFO, CRC included");
    CHECK(nfc_rd(&a, 0x09) == 0x03 && nfc_rd(&a, 0x09) == 0xD5,
          "and reads back in order");

    /* Incoming frames must not splice into a nonempty FIFO. */
    nfc_wr(&a, 0x0A, 0x80); nfc_reply(&a);
    nfc_wr(&a, 0x09, 0x11); nfc_reply(&a);     /* A mid-composition */
    uint64_t dropped = a.nfc.rf_dropped;
    nfc_wr(&b, 0x0A, 0x80); nfc_reply(&b);
    nfc_wr(&b, 0x09, 0x01); nfc_reply(&b);
    nfc_wr(&b, 0x01, 0x0C); nfc_reply(&b);
    nfc_wr(&b, 0x0D, 0x80); nfc_reply(&b);
    CHECK(a.nfc.rf_dropped == dropped + 1,
          "a busy FIFO refuses the frame instead of splicing");
    CHECK(nfc_rd(&a, 0x0A) == 1, "the composed byte is untouched");

    /* With the field off, StartSend holds the frame while listening. */
    uint64_t b_heard_before = b.nfc.rf_rcvd;
    nfc_wr(&a, 0x14, 0x80); nfc_reply(&a);     /* field off */
    nfc_wr(&a, 0x0A, 0x80); nfc_reply(&a);
    for (int i = 0; i < 17; i++) { nfc_wr(&a, 0x09, atr_req[i]); nfc_reply(&a); }
    nfc_wr(&a, 0x01, 0x0C); nfc_reply(&a);
    nfc_wr(&a, 0x0D, 0x80); nfc_reply(&a);
    CHECK(a.nfc.pend_n == 17, "the frame is held, not sent");
    CHECK(b.nfc.rf_rcvd == b_heard_before, "nothing reached the peer yet");
    /* A hears B while its own frame is held. */
    nfc_wr(&b, 0x0A, 0x80); nfc_reply(&b);
    nfc_wr(&b, 0x09, 0x11); nfc_reply(&b);
    nfc_wr(&b, 0x01, 0x0C); nfc_reply(&b);
    nfc_wr(&b, 0x0D, 0x80); nfc_reply(&b);
    CHECK(nfc_rd(&a, 0x0A) == 3, "the holder heard the poller's frame (+CRC)");
    /* Idle cancels the held frame. */
    nfc_wr(&a, 0x01, 0x00); nfc_reply(&a);
    CHECK(a.nfc.pend_n == 0, "Idle cancels the held frame");

    /* Without interference, the held frame sends after its delay. */
    nfc_wr(&a, 0x0A, 0x80); nfc_reply(&a);
    for (int i = 0; i < 17; i++) { nfc_wr(&a, 0x09, atr_req[i]); nfc_reply(&a); }
    nfc_wr(&a, 0x01, 0x0C); nfc_reply(&a);
    nfc_wr(&a, 0x0D, 0x80); nfc_reply(&a);
    nfc_wr(&b, 0x0A, 0x80); nfc_reply(&b);     /* clear B, then listen */
    nfc_wr(&b, 0x01, 0x08); nfc_reply(&b);
    uint64_t heard = b.nfc.rf_rcvd;
    a.cycles = a.nfc.pend_at + 1;              /* the quiet-air slot arrives */
    periph_tick(&a, 1);
    CHECK(a.nfc.pend_n == 0 && b.nfc.rf_rcvd == heard + 1,
          "the held frame launches after its slot and the listener hears it");
}

/* Test both raw-DEP and LLCP phone-injection sessions from the initiator side. */
static void np_feed(Emu *e, const uint8_t *f, int n)
{
    nfcpeer_rf_in(e, f, n);
    e->cycles = e->nfc_vpeer->out_at;      /* jump to the reply's slot */
    nfcpeer_tick(e);
}

static void test_nfcpeer(void)
{
    const DeviceProfile *d4 = device_find("4u");
    if (!d4) return;
    static Emu e;
    memset(&e, 0, sizeof e);
    e.dev = *d4;
    e.cmu.osc3_hz = 30e6;
    static uint8_t rom[PS_ROM_SIZE];
    e.rom = rom;
    e.ir_fast = true;

    char tmp[512];
    const char *td = getenv("TEMP");
    snprintf(tmp, sizeof tmp, "%s/tamaemu_nfcpeer_test.bin", td ? td : ".");
    FILE *f = fopen(tmp, "wb");
    if (!f) { CHECK(0, "cannot write the test payload"); return; }
    /* nfcpeer only requires the TAMAGO magic. */
    uint8_t item[300];
    memset(item, 0, sizeof item);
    memcpy(item, "TAMAGO", 6);
    fwrite(item, 1, sizeof item, f);
    fclose(f);
    CHECK(nfcpeer_open(&e, tmp) == 1, "the payload loads");
    if (!e.nfc_vpeer) return;
    NfcPeer *p = e.nfc_vpeer;

    /* Device-to-device DEP without general bytes. */
    static const uint8_t atr_raw[17] = {0x11, 0xD4, 0x00, 0xF0, 0x40, 0x31,
        0x18, 0xB0, 0x45, 0x46, 0x66, 0xF0, 0x40, 0x00, 0x00, 0x00, 0x00};
    np_feed(&e, atr_raw, sizeof atr_raw);
    CHECK(!p->llcp, "PPi 0 with no general bytes reads as a raw-DEP peer");
    CHECK(p->out_n >= 18 && p->out[1] == 0xD5 && p->out[2] == 0x01,
          "and is answered with an ATR_RES");
    CHECK(p->out[3] == 0xF0 && p->out[4] == 0x40,
          "whose NFCID3t carries the 4U signature the tama's dispatch demands");

    /* A raw push is acknowledged by a bare DEP_RES with the same PNI. */
    static const uint8_t rec[8] = {0x08, 0xD4, 0x06, 0x02, 0x68, 0x01, 0x00, 0x00};
    np_feed(&e, rec, sizeof rec);
    CHECK(p->out_n == 4 && p->out[1] == 0xD5 && p->out[2] == 0x07 &&
          p->out[3] == 0x02, "a raw record gets a bare DEP_RES for its PNI");

    /* App peer with LLCP in the general bytes. */
    static const uint8_t atr_llcp[23] = {
        0x17, 0xD4, 0x00, 0xF0, 0x40, 0x31, 0x18, 0xB0, 0x45, 0x46, 0x66,
        0xF0, 0x40,             /* NFCID3i */
        0x00,                   /* DIDi */
        0x00, 0x00,             /* BSi BRi */
        0x02,                   /* PPi: general bytes present */
        0x46, 0x66, 0x6D,       /* "Ffm" */
        0x01, 0x01, 0x13,       /* VERSION */
    };
    np_feed(&e, atr_llcp, sizeof atr_llcp);
    CHECK(p->llcp, "PPi bit 1 plus the Ffm magic reads as an LLCP peer");

    /* SYMM polling prompts the phone to open SNEP. */
    static const uint8_t symm[6] = {0x06, 0xD4, 0x06, 0x00, 0x00, 0x00};
    np_feed(&e, symm, sizeof symm);
    CHECK(p->out_n == 6, "the reply to SYMM is one LLCP PDU");
    unsigned hdr = ((unsigned)p->out[4] << 8) | p->out[5];
    CHECK(((hdr >> 6) & 0xF) == 4, "which is a CONNECT");
    CHECK(((hdr >> 10) & 0x3F) == 4, "to the well-known SNEP SAP 4");
    CHECK((hdr & 0x3F) == 32, "from our SAP 32");

    /* CC prompts the first SNEP PUT fragment. */
    static const uint8_t cc[6] = {0x06, 0xD4, 0x06, 0x01, 0x81, 0x84};
    np_feed(&e, cc, sizeof cc);
    hdr = ((unsigned)p->out[4] << 8) | p->out[5];
    CHECK(((hdr >> 6) & 0xF) == 12, "CC is followed by an I-PDU");
    CHECK(p->out[7] == 0x10 && p->out[8] == 0x02,
          "carrying a SNEP PUT: %02x %02x", p->out[7], p->out[8]);
    /* SNEP length is the NDEF message: C2 header (6) + type (38) + payload.
     * The handshake tap is 258 bytes padded to 260. */
    unsigned snep_len = ((unsigned)p->out[9] << 24) | ((unsigned)p->out[10] << 16) |
                        ((unsigned)p->out[11] << 8) | p->out[12];
    CHECK(snep_len == 6 + 38 + 260,
          "whose NDEF length is the padded tap plus the record header: %u", snep_len);
    CHECK(p->out[13] == 0xC2 && p->out[14] == 0x26,
          "long-form MIME record header: %02x %02x", p->out[13], p->out[14]);
    CHECK(p->out_n == 4 + 3 + 125, "and the fragment is a full 125-byte MIU");

    /* Continue requests advance the fragmented transfer. */
    int sent_after_first = p->tx_sent;
    /* I-PDU from SAP 4 to our SAP 32 (header 83 04), N(S)=1 N(R)=0, carrying
     * a 6-byte SNEP CONTINUE */
    static const uint8_t cont[13] = {0x0D, 0xD4, 0x06, 0x02, 0x83, 0x04, 0x10,
                                     0x10, 0x80, 0x00, 0x00, 0x00, 0x00};
    np_feed(&e, cont, sizeof cont);
    CHECK(p->tx_sent > sent_after_first, "SNEP CONTINUE draws the next fragment");
    remove(tmp);
}

/* Each profile maps its declared regions and nothing else. */
static void test_mem_map_dev(const DeviceProfile *d)
{
    Emu *e = fresh_dev(d);
    CHECK(d->rom_size <= PS_ROM_SIZE,
          "%s: test ROM buffer holds this machine's %u-byte image", d->name, d->rom_size);

    mem_write32(e, d->a0ram_base + 0x1000, 0xDEADBEEF);
    CHECK(mem_read32(e, d->a0ram_base + 0x1000) == 0xDEADBEEF, "%s: a0ram roundtrip", d->name);
    CHECK(mem_read8 (e, d->a0ram_base + 0x1000) == 0xEF, "%s: a0ram little endian", d->name);

    mem_write32(e, d->ivram_base, 0xCAFEF00D);
    CHECK(mem_read32(e, d->ivram_base) == 0xCAFEF00D, "%s: ivram roundtrip", d->name);

    mem_write8(e, d->dstram_base + 4, 0x5A);
    CHECK(mem_read8(e, d->dstram_base + 4) == 0x5A, "%s: dstram roundtrip", d->name);

    /* 0x300500 is plain register-backed I/O. */
    mem_write8(e, d->io_base + 0x500, 0x3C);
    CHECK(mem_read8(e, d->io_base + 0x500) == 0x3C, "%s: io register-RAM roundtrip", d->name);

    /* ROM reads use this profile's base. */
    e->rom[0] = 0x12; e->rom[1] = 0x34;
    CHECK(mem_read16(e, d->rom_base) == 0x3412, "%s: ROM reads at %08x", d->name, d->rom_base);
    CHECK(mem_read8(e, d->rom_base + d->rom_size - 1) == 0xFF,
          "%s: last ROM byte is reachable", d->name);

    /* Addresses outside every region read as unmapped. */
    CHECK(mem_read8(e, 0x00500000) == 0, "%s: unmapped reads 0", d->name);
}

/* AMD/JEDEC unlock is relative to the selected device's ROM base. */
static void test_flash_dev(const DeviceProfile *d)
{
    Emu *e = fresh_dev(d);
    uint32_t rb = d->rom_base;

    mem_write16(e, rb + 0xAAA, 0xAA);
    mem_write16(e, rb + 0x554, 0x55);
    mem_write16(e, rb + 0xAAA, 0xA0);
    mem_write16(e, rb + 0x2000, 0x0102);
    CHECK(e->rom[0x2000] == 0x02 && e->rom[0x2001] == 0x01,
          "%s: program landed at ROM+0x2000 (%02x %02x)", d->name,
          e->rom[0x2000], e->rom[0x2001]);
    CHECK(e->flash_programs == 1, "%s: one program counted: %llu", d->name,
          (unsigned long long)e->flash_programs);
    CHECK(e->flash_dirty, "%s: programming marks the image dirty", d->name);

    /* sector erase puts the 0xFF back */
    e->flash_busy = 0;
    mem_write16(e, rb + 0xAAA, 0xAA);
    mem_write16(e, rb + 0x554, 0x55);
    mem_write16(e, rb + 0xAAA, 0x80);
    mem_write16(e, rb + 0xAAA, 0xAA);
    mem_write16(e, rb + 0x554, 0x55);
    mem_write16(e, rb + 0x2000, 0x30);
    CHECK(e->flash_erases == 1, "%s: one erase counted: %llu", d->name,
          (unsigned long long)e->flash_erases);
    CHECK(e->rom[0x2000] == 0xFF, "%s: erase restored 0xFF: %02x", d->name, e->rom[0x2000]);

    /* Unlock cycles at another device's base do nothing. */
    Emu *f = fresh_dev(d);
    uint32_t wrong = (rb == PS_ROM_BASE) ? 0x00C00000u : PS_ROM_BASE;
    mem_write16(f, wrong + 0xAAA, 0xAA);
    mem_write16(f, wrong + 0x554, 0x55);
    mem_write16(f, wrong + 0xAAA, 0xA0);
    mem_write16(f, wrong + 0x2000, 0x0102);
    CHECK(f->flash_programs == 0,
          "%s: unlock aimed at %08x programs nothing here: %llu", d->name, wrong,
          (unsigned long long)f->flash_programs);
}

static void test_cpu_reset_vector_dev(const DeviceProfile *d)
{
    Emu *e = fresh_dev(d);
    uint32_t entry = d->rom_base + 0x1234;
    uint32_t off = d->ttbr_reset - d->rom_base;
    e->rom[off + 0] = (uint8_t)entry;         e->rom[off + 1] = (uint8_t)(entry >> 8);
    e->rom[off + 2] = (uint8_t)(entry >> 16); e->rom[off + 3] = (uint8_t)(entry >> 24);
    cpu_reset(e);
    CHECK(e->ttbr == d->ttbr_reset, "%s: ttbr reset to %08x, got %08x",
          d->name, d->ttbr_reset, e->ttbr);
    CHECK(e->pc == entry, "%s: reset vector read from this machine's NOR: want %08x got %08x",
          d->name, entry, e->pc);
}

/* Build an 8 MiB image with one kind-7 game in slot 0. */
static uint8_t *fake_image_with_game(const DlcDevice *d)
{
    const DlcKind *k = dlc_kind(d, 7);
    if (!k) return NULL;
    uint8_t *img = malloc(DLC_IMAGE_SIZE);
    memset(img, 0xFF, DLC_IMAGE_SIZE);
    uint8_t *rec = img + k->base;
    memcpy(rec, "TAMAGO", 6);
    /* name: UTF-16BE "AB" at 0x06, ascii id at 0x34, type byte at 0x4E */
    rec[0x06] = 0x00; rec[0x07] = 'A';
    rec[0x08] = 0x00; rec[0x09] = 'B';
    memcpy(rec + 0x34, "idn_gm00200_1", sizeof("idn_gm00200_1")); /* incl. NUL */
    rec[0x4E] = 0x94;
    return img;
}

static void test_dlc_slots(void)
{
    const DlcDevice *d = dlc_device_find("ps");
    CHECK(d != NULL, "ps device exists");
    uint8_t *img = fake_image_with_game(d);
    CHECK(img != NULL, "fake image built (kind 7 exists on ps)");
    if (!img) return;
    DlcSlot s[4];
    int n = dlc_store_slots(d, img, 7, s, 4);
    CHECK(n == 2, "kind 7 has 2 slots, got %d", n);
    CHECK(s[0].occupied == 1, "slot 0 occupied");
    CHECK(s[1].occupied == 0, "slot 1 empty");
    CHECK(strcmp(s[0].id, "idn_gm00200_1") == 0, "slot 0 id = %s", s[0].id);
    CHECK(s[0].off == (long)dlc_kind(d, 7)->base, "slot 0 offset");
    CHECK(s[1].off == (long)(dlc_kind(d, 7)->base + 0x8000), "slot 1 offset");
    CHECK(dlc_store_slots(d, img, 7, s, 1) == 1, "max caps the output");
    free(img);
}

static void test_dlc_free_slot(void)
{
    const DlcDevice *d = dlc_device_find("ps");
    const DlcKind *k = dlc_kind(d, 7);
    uint8_t *img = fake_image_with_game(d);
    CHECK(img != NULL && k != NULL, "fixture built");
    if (!img || !k) { free(img); return; }

    CHECK(dlc_free_slot(d, img, 7, 0) == 0, "free slot 0 succeeds");

    DlcSlot s[4];
    dlc_store_slots(d, img, 7, s, 4);
    CHECK(s[0].occupied == 0, "slot 0 now empty");

    /* Freeing a slot erases its full span. */
    int all_ff = 1;
    for (size_t i = 0; i < k->slotsz; i++)
        if (img[k->base + i] != 0xFF) { all_ff = 0; break; }
    CHECK(all_ff, "whole slot is 0xFF");

    CHECK(dlc_free_slot(d, img, 7, 9) != 0, "out-of-range slot refused");
    CHECK(dlc_free_slot(d, img, 7, -1) != 0, "negative slot refused");
    CHECK(dlc_free_slot(d, img, 99, 0) != 0, "unknown kind refused");
    free(img);
}

static void test_dlc_place_at(void)
{
    const DlcDevice *d = dlc_device_find("ps");
    const DlcKind *k = dlc_kind(d, 7);
    uint8_t *img = fake_image_with_game(d);
    CHECK(img != NULL && k != NULL, "fixture built");
    if (!img || !k) { free(img); return; }

    uint8_t rec[0x60];
    memset(rec, 0, sizeof rec);
    memcpy(rec, "TAMAGO", 6);
    memcpy(rec + 0x34, "idn_gm00999_1", sizeof("idn_gm00999_1"));

    /* Replacing an occupied slot requires an explicit free first. */
    CHECK(dlc_place_at(d, img, 7, 0, rec, sizeof rec) == -1,
          "occupied slot refused");

    long off = dlc_place_at(d, img, 7, 1, rec, sizeof rec);
    CHECK(off == (long)(k->base + k->slotsz), "placed at slot 1, got %ld", off);

    DlcSlot s[4];
    dlc_store_slots(d, img, 7, s, 4);
    CHECK(s[1].occupied == 1, "slot 1 now occupied");
    CHECK(strcmp(s[1].id, "idn_gm00999_1") == 0, "slot 1 id = %s", s[1].id);

    dlc_free_slot(d, img, 7, 0);
    CHECK(dlc_place_at(d, img, 7, 0, rec, sizeof rec) == (long)k->base,
          "freed slot accepts a record");

    CHECK(dlc_place_at(d, img, 7, 1, rec, k->slotsz + 1) == -1,
          "oversized payload refused");
    CHECK(dlc_place_at(d, img, 7, 9, rec, sizeof rec) == -1,
          "out-of-range slot refused");
    CHECK(dlc_place_at(d, img, 99, 0, rec, sizeof rec) == -1,
          "unknown kind refused");
    free(img);
}

/* Validate every device row against that device's own image size. */
static void test_dlc_device_rows(void)
{
    for (int i = 0; i < dlc_device_count(); i++) {
        const DlcDevice *d = dlc_device_at(i);
        CHECK(dlc_device_check(d, stderr), "%s: device row is consistent", d->name);
    }

    const DlcDevice *id = dlc_device_find("id");
    CHECK(id != NULL, "id device exists");
    if (!id) return;
    CHECK(dlc_image_size(id) == 4u * 1024u * 1024u, "iD image is 4 MB");
    CHECK(dlc_cat_off(id) == 0x4F, "iD category byte is at 0x4F");
    CHECK(dlc_image_size(dlc_device_find("ps")) == 8u * 1024u * 1024u,
          "the default stays 8 MB");
    CHECK(dlc_cat_off(dlc_device_find("ps")) == 0x50,
          "the default category offset stays 0x50");
}

/* Check each confirmed iD route and keep doughnuts unrouted. */
static void test_dlc_route_id(void)
{
    const DlcDevice *id = dlc_device_find("id");
    static const struct { int typ, cat, flag, kind; } want[] = {
        { 0x01, 0x01, 0x01, 0 },   /* meal        */
        { 0x01, 0x01, 0x03, 1 },   /* snack       */
        { 0x01, 0x04, 0x01, 2 },   /* toy         */
        { 0x01, 0x06, 0x00, 3 },   /* letter      */
        { 0x01, 0x07, 0x00, 4 },   /* wallpaper   */
        { 0x01, 0x02, 0x00, 8 },   /* accessory   */
        { 0x02, 0x03, 0x01, 6 },   /* outfit      */
        { 0x02, 0x03, 0x02, 7 },   /* backdrop    */
        { 0x01, 0x01, 0x02, -1 },  /* doughnut - parked, no store found */
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        const char *label = NULL;
        int kind = -2;
        dlc_route(id, want[i].typ, want[i].cat, "", want[i].flag, NULL, 0,
                  &label, &kind);
        CHECK(kind == want[i].kind, "id route %02x %02x %02x -> kind %d, got %d",
              want[i].typ, want[i].cat, want[i].flag, want[i].kind, kind);
        CHECK(label != NULL, "id route %02x %02x %02x carries a label",
              want[i].typ, want[i].cat, want[i].flag);
    }

    /* iD games use discriminator 0x37; other values route to outings. */
    uint8_t rec[0x80];
    memset(rec, 0, sizeof rec);
    const char *label;
    int kind;

    rec[DLC_ID_GAME_DISC_OFF] = DLC_ID_GAME_DISC_VAL;
    label = NULL; kind = -2;
    dlc_route(id, 0x14, 0x02, "", 0x00, rec, sizeof rec, &label, &kind);
    CHECK(kind == 5 && label && !strcmp(label, "game"),
          "0x64 == 0x37 is a game (kind %d, %s)", kind, label ? label : "-");

    rec[DLC_ID_GAME_DISC_OFF] = 0x0F;
    label = NULL; kind = -2;
    dlc_route(id, 0x14, 0x02, "", 0x00, rec, sizeof rec, &label, &kind);
    CHECK(kind == 9 && label && !strcmp(label, "outing"),
          "0x64 == 0x0f is an outing (kind %d, %s)", kind, label ? label : "-");

    /* Unseen discriminator values still take the outing branch. */
    rec[DLC_ID_GAME_DISC_OFF] = 0x99;
    label = NULL; kind = -2;
    dlc_route(id, 0x14, 0x02, "", 0x00, rec, sizeof rec, &label, &kind);
    CHECK(kind == 9, "an unseen 0x64 follows the firmware to Travel, got %d", kind);

    /* A record too short for the discriminator has no route. */
    label = NULL; kind = -2;
    dlc_route(id, 0x14, 0x02, "", 0x00, rec, 0x20, &label, &kind);
    CHECK(kind == -1 && label != NULL, "a truncated record routes nowhere");

    /* iD stores games as kind 5. */
    CHECK(dlc_game_kind(id) == 5, "iD games are kind 5, got %d", dlc_game_kind(id));
    CHECK(dlc_game_kind(dlc_device_find("ps")) == 7, "the default stays 7");
}

/* Build a minimal iD game/outing record with its discriminator and sum16.
 * tag changes the covered ASCII ID field before the checksum is written. */
static void write_id_payload(const char *path, uint8_t disc, char tag)
{
    uint8_t p[0x80];
    memset(p, 0, sizeof p);
    memcpy(p, "TAMAGO", 6);
    if (tag) p[0x34] = (uint8_t)tag;
    p[0x4B] = sizeof p;                       /* u32be record length at 0x48 */
    p[0x4C] = 0xCD; p[0x4D] = 0x80;
    p[0x4E] = 0x14; p[0x4F] = 0x02; p[0x50] = 0x01; p[0x51] = 0x00;
    p[DLC_ID_GAME_DISC_OFF] = disc;
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof p - 2; i++) sum += p[i];
    p[sizeof p - 2] = (uint8_t)(sum >> 8);
    p[sizeof p - 1] = (uint8_t)sum;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(p, 1, sizeof p, f); fclose(f); }
}

/* Route two otherwise identical records by the discriminator at 0x64. */
static void test_dlc_inject_id_game_outing(void)
{
    const DlcDevice *id = dlc_device_find("id");
    const char *sav = "id_inject_test.tmp";
    const char *p1 = "id_payload_a.tmp", *p2 = "id_payload_b.tmp";

    uint8_t *img = malloc(4u * 1024u * 1024u);
    memset(img, 0xFF, 4u * 1024u * 1024u);
    /* Set the iD reset vector so device matching is exercised. */
    img[0] = 0xFA; img[1] = 0x07; img[2] = 0xC9; img[3] = 0x00;
    FILE *f = fopen(sav, "wb");
    CHECK(f != NULL, "4 MB fixture image created");
    if (f) { fwrite(img, 1, 4u * 1024u * 1024u, f); fclose(f); }
    free(img);

    write_id_payload(p1, DLC_ID_GAME_DISC_VAL, 0);   /* a game   */
    write_id_payload(p2, 0x0F, 0);                   /* an outing */
    const char *paths[2] = { p1, p2 };
    DlcResult res[2];
    char err[256];

    CHECK(dlc_inject(id, sav, paths, 2, 0, 0, NULL, 0, res, err, sizeof err) == 0,
          "inject runs on a 4 MB image: %s", err);
    CHECK(res[0].off == 0x360000, "game landed at the Game Centre: %08lx", res[0].off);
    CHECK(res[1].off == 0x0E0000, "outing landed at Travel: %08lx", res[1].off);
    CHECK(res[0].kind == 5 && res[1].kind == 9, "each went to its own store");

    CHECK(dlc_inject(id, sav, paths, 2, 0, 0, NULL, 0, res, err, sizeof err) == 0,
          "re-run runs");
    CHECK(strstr(res[0].error, "already installed") != NULL &&
          strstr(res[1].error, "already installed") != NULL,
          "re-run is a skip, not a duplicate");

    /* Replacement requires one explicitly freed slot. */
    const char *p3 = "id_payload_c.tmp", *p4 = "id_payload_d.tmp";
    write_id_payload(p3, DLC_ID_GAME_DISC_VAL, 'Y');
    write_id_payload(p4, DLC_ID_GAME_DISC_VAL, 'Z');
    DlcResult r1[1];

    const char *fill[1] = { p3 };
    CHECK(dlc_inject(id, sav, fill, 1, 0, 0, NULL, 0, r1, err, sizeof err) == 0 &&
          r1[0].off == 0x368000,
          "the second game fills the store: %08lx", r1[0].off);

    const char *one[1] = { p4 };
    CHECK(dlc_inject(id, sav, one, 1, 0, 0, NULL, 0, r1, err, sizeof err) == 0 &&
          r1[0].off < 0 && strstr(r1[0].error, "store full") != NULL,
          "a full store refuses a third without a free: %s", r1[0].error);

    DlcFree fr = { 5, 0 };                      /* the iD keeps games at kind 5 */
    CHECK(dlc_kind(id, 5) != NULL, "kind 5 is the store this test frees");
    CHECK(dlc_inject(id, sav, one, 1, 0, 0, &fr, 1, r1, err, sizeof err) == 0 &&
          r1[0].off == 0x360000,
          "freeing slot 0 lets it in, at that slot: %08lx", r1[0].off);

    remove(sav); remove(p1); remove(p2); remove(p3); remove(p4);
}

/* A valid full-record checksum may override an understated length at +0x48. */
static void test_dlc_extract_short_length_field(void)
{
    const char *path = "extract_len_test.tmp";
    enum { N = 0x200, DECLARED = 0x100 };
    uint8_t p[N];
    memset(p, 0, sizeof p);
    memcpy(p, "TAMAGO", 6);
    p[0x48] = 0; p[0x49] = 0;
    p[0x4A] = (uint8_t)(DECLARED >> 8); p[0x4B] = (uint8_t)DECLARED;
    for (int i = 0x60; i < N - 2; i++) p[i] = (uint8_t)(i * 7);
    unsigned sum = 0;
    for (int i = 0; i < N - 2; i++) sum += p[i];
    p[N - 2] = (uint8_t)(sum >> 8); p[N - 1] = (uint8_t)sum;

    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "extract fixture created");
    if (f) { fwrite(p, 1, sizeof p, f); fclose(f); }

    uint8_t *out = NULL; size_t outlen = 0; char err[DLC_ERR_MAX] = "";
    CHECK(dlc_extract_payload(path, &out, &outlen, err, sizeof err) == 0 &&
          outlen == (size_t)N,
          "an undercounting 0x48 is overridden by the MAC: len=%zu err='%s'",
          outlen, err);
    free(out); out = NULL;

    /* The fallback length scan must still reject a corrupt body. */
    p[0x70] ^= 0xFF;
    f = fopen(path, "wb");
    if (f) { fwrite(p, 1, sizeof p, f); fclose(f); }
    CHECK(dlc_extract_payload(path, &out, &outlen, err, sizeof err) != 0,
          "a corrupt record is still rejected");
    free(out);
    remove(path);
}

static void test_devices(void)
{
    test_device_table();
    for (size_t i = 0; ; i++) {
        const DeviceProfile *d = device_at(i);
        if (!d) break;
        test_mem_map_dev(d);
        test_flash_dev(d);
        test_cpu_reset_vector_dev(d);
    }
}

static void test_swapreq_roundtrip(void)
{
    uint8_t body[64];
    for (int i = 0; i < 64; i++) body[i] = (uint8_t)i;

    /* Use a relative path so the test works on Windows and POSIX. */
    const char *tf = "swapreq_test.tmp";

    SwapWrite w[2] = {
        { 0x710000, 0x8000, NULL },     /* free slot 0: NULL means 0xFF fill */
        { 0x718000, 64,     body },
    };
    CHECK(swapreq_write(tf, w, 2) == 0, "write ok");

    SwapReq r;
    CHECK(swapreq_read(tf, &r) == 0, "read ok");
    CHECK(r.count == 2, "two entries, got %d", r.count);
    CHECK(r.w[0].off == 0x710000 && r.w[0].len == 0x8000, "entry 0");
    CHECK(r.w[0].bytes == NULL, "entry 0 is a fill");
    CHECK(r.w[1].len == 64 && r.w[1].bytes && r.w[1].bytes[5] == 5, "entry 1 payload");
    swapreq_free(&r);

    /* Rewrite the file shorter because UCRT has no truncate(). */
    {
        uint8_t whole[4096];
        FILE *f = fopen(tf, "rb");
        CHECK(f != NULL, "reopen for truncation");
        size_t got = f ? fread(whole, 1, sizeof whole, f) : 0;
        if (f) fclose(f);
        CHECK(got > 8, "read the request back, %u bytes", (unsigned)got);
        f = fopen(tf, "wb");
        if (f) { fwrite(whole, 1, got - 8, f); fclose(f); }
    }
    CHECK(swapreq_read(tf, &r) != 0, "truncated file rejected");

    /* A wrong magic must be rejected too. */
    {
        FILE *f = fopen(tf, "wb");
        if (f) { fwrite("NOTASWAP", 1, 8, f); fwrite(body, 1, 32, f); fclose(f); }
    }
    CHECK(swapreq_read(tf, &r) != 0, "bad magic rejected");

    remove(tf);
}

/* --keys validator: 3 chars of a-z/1-9, case-folded, no repeats, s/n reserved
 * (they are the stay-awake and NFC-touch keys in the SDL window). */
static void test_keys_parse(void)
{
    char k[3] = {'z', 'x', 'c'};
    CHECK(panel_keys_parse("jkl", k) == 1, "jkl accepted");
    CHECK(k[0] == 'j' && k[1] == 'k' && k[2] == 'l', "jkl stored");
    CHECK(panel_keys_parse("JKL", k) == 1 && k[0] == 'j', "upper case folded to lower");
    CHECK(panel_keys_parse("a19", k) == 1 && k[2] == '9', "digits 1-9 accepted");
    char before[3] = {'a', 'b', 'd'};
    memcpy(k, before, 3);
    CHECK(panel_keys_parse("aab", k) == 0, "duplicate rejected");
    CHECK(panel_keys_parse("aAb", k) == 0, "duplicate across case rejected");
    CHECK(panel_keys_parse("ab",  k) == 0, "too short rejected");
    CHECK(panel_keys_parse("abcd", k) == 0, "too long rejected");
    CHECK(panel_keys_parse("ab0", k) == 0, "0 rejected (reserved: back to real time)");
    CHECK(panel_keys_parse("sab", k) == 0, "s rejected (reserved: stay-awake)");
    CHECK(panel_keys_parse("abn", k) == 0, "n rejected (reserved: NFC touch)");
    CHECK(panel_keys_parse("a-b", k) == 0, "punctuation rejected");
    CHECK(panel_keys_parse("",    k) == 0, "empty rejected");
    CHECK(panel_keys_parse(NULL,  k) == 0, "NULL rejected");
    CHECK(memcmp(k, before, 3) == 0, "failed parse leaves out[] untouched");
}

int main(void)
{
    test_devices();
    test_dlc_device_rows();
    test_dlc_route_id();
    test_dlc_inject_id_game_outing();
    test_dlc_extract_short_length_field();
    test_dlc_slots();
    test_dlc_free_slot();
    test_dlc_place_at();
    test_ir_mode();
    test_ir_gpio();
    test_pn512();
    test_pn512_rf_link();
    test_nfcpeer();
    test_link();
    test_link_hub_relay();
    test_link_collision();
    test_link_wire_time();
    test_ir_rx_wire_time();
    test_rtc_buttons();
    test_mem();
    test_cpu_alu();
    test_cpu_ext();
    test_cpu_xjp_patch_bytes();
    test_cpu_branch_call();
    test_cpu_bad_pc_trap();
    test_watch_flash();
    test_cmu();
    test_lockstep();
    test_route_key();
    test_speed_step();
    test_panel_hit();
    test_keys_parse();
    test_swapreq_roundtrip();
    if (fails) { fprintf(stderr, "%d FAILURES\n", fails); return 1; }
    fprintf(stderr, "all tests passed\n");
    return 0;
}
