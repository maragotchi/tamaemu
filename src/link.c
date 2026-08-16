#include "emu.h"
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
static int wsa_ready;
static void wsa_init(void)
{
    if (!wsa_ready) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); wsa_ready = 1; }
}
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
static void wsa_init(void) {}
#endif

static void sock_setup(uintptr_t s)
{
    int one = 1;
    setsockopt((int)s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef _WIN32
    u_long nb = 1; ioctlsocket((SOCKET)s, FIONBIO, &nb);
#else
    fcntl((int)s, F_SETFL, fcntl((int)s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

/* Separate processes use host time because their emulated clocks are independent. */
uint64_t link_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

static void rec_put(uint8_t *r, uint32_t id, uint8_t b, uint16_t dur, uint64_t t)
{
    r[0] = (uint8_t)id;
    r[1] = (uint8_t)(id >> 8);
    r[2] = (uint8_t)(id >> 16);
    r[3] = (uint8_t)(id >> 24);
    r[4] = b;
    r[5] = 0;
    r[6] = (uint8_t)dur;
    r[7] = (uint8_t)(dur >> 8);
    for (int i = 0; i < 8; i++) r[8 + i] = (uint8_t)(t >> (8 * i));
}

static uint32_t rec_id(const uint8_t *r)
{
    return (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) |
           ((uint32_t)r[3] << 24);
}

static uint8_t  rec_byte(const uint8_t *r) { return r[4]; }
static uint16_t rec_dur(const uint8_t *r)  { return (uint16_t)(r[6] | (r[7] << 8)); }

static uint64_t rec_t(const uint8_t *r)
{
    uint64_t t = 0;
    for (int i = 7; i >= 0; i--) t = (t << 8) | r[8 + i];
    return t;
}

/* Check released and pending transmissions so both sides of a collision fail. */
static int coll_overlaps(Link *l, uint32_t id, uint64_t t0, uint64_t t1, int skip)
{
    for (int i = 0; i < l->seen_n; i++) {
        if (l->seen_id[i] == id) continue;
        if (t0 < l->seen_t1[i] && l->seen_t0[i] < t1) return 1;
    }
    for (int i = 0; i < l->pend_n; i++) {
        if (i == skip) continue;
        if (rec_id(l->pend[i]) == id) continue;
        uint64_t o0 = rec_t(l->pend[i]), o1 = o0 + rec_dur(l->pend[i]);
        if (t0 < o1 && o0 < t1) return 1;
    }
    return 0;
}

/* LINK_LEAD_US buffers jitter while sender clock offsets are estimated. */
#define LINK_OFF_DECAY_SHIFT 10        /* ~1 ms of recovery per second elapsed */

static int org_find(Link *l, uint32_t id)
{
    for (int i = 0; i < l->org_n; i++)
        if (l->org_id[i] == id) return i;
    return -1;
}

static void org_sample(Link *l, uint32_t id, uint64_t t_us, uint64_t now)
{
    int64_t s = (int64_t)(now - t_us);
    int i = org_find(l, id);
    if (i < 0) {
        if (l->org_n < LINK_ORG_N) {
            i = l->org_n++;
        } else {
            i = 0;
            for (int k = 1; k < LINK_ORG_N; k++)
                if (l->org_upd[k] < l->org_upd[i]) i = k;
        }
        l->org_id[i] = id;
        l->org_off[i] = s;
        l->org_upd[i] = now;
        return;
    }
    l->org_off[i] += (int64_t)(now - l->org_upd[i]) >> LINK_OFF_DECAY_SHIFT;
    if (s < l->org_off[i]) l->org_off[i] = s;
    l->org_upd[i] = now;
}

/* Records without a clock estimate are due immediately. */
static int64_t rec_due(Link *l, const uint8_t *r)
{
    int i = org_find(l, rec_id(r));
    if (i < 0) return 0;
    return (int64_t)rec_t(r) + l->org_off[i] + LINK_LEAD_US;
}

static void coll_remember(Link *l, uint32_t id, uint64_t t0, uint64_t t1)
{
    int i = l->seen_n < LINK_COLL_HIST ? l->seen_n++ : 0;
    if (l->seen_n >= LINK_COLL_HIST) {
        for (int k = 1; k < LINK_COLL_HIST; k++) {
            l->seen_t0[k - 1] = l->seen_t0[k];
            l->seen_t1[k - 1] = l->seen_t1[k];
            l->seen_id[k - 1] = l->seen_id[k];
        }
        i = LINK_COLL_HIST - 1; l->seen_n = LINK_COLL_HIST;
    }
    l->seen_t0[i] = t0; l->seen_t1[i] = t1; l->seen_id[i] = id;
}

static void net_add_sock(Link *l, uintptr_t s)
{
    if (l->nsock >= LINK_PEERS_MAX) { CLOSESOCK((int)s); return; }
    sock_setup(s);
    l->peer[l->nsock++] = s;
    l->net = 1;
}

static void net_drop_sock(Link *l, int i)
{
    CLOSESOCK((int)l->peer[i]);
    for (int k = i; k + 1 < l->nsock; k++) l->peer[k] = l->peer[k + 1];
    l->nsock--;
    fprintf(stderr, "[link] a peer disconnected (%d left)\n", l->nsock);
}

/* Accept pending peers without closing the hub listener. */
static void net_accept_pending(Link *l)
{
    if (!l->listening) return;
    for (;;) {
        if (l->nsock >= LINK_PEERS_MAX - 1) return;   /* room for us + guests */
        uintptr_t cs = (uintptr_t)accept((int)l->listener, NULL, NULL);
        if (cs == (uintptr_t)INVALID_SOCKET) return;
        net_add_sock(l, cs);
        fprintf(stderr, "[link] another device joined - %d now on the bus\n", l->nsock + 1);
    }
}

static void net_send_all(Link *l, int except_i, const uint8_t *b, int n)
{
    for (int i = 0; i < l->nsock; i++) {
        if (i == except_i) continue;
        if (send((int)l->peer[i], (const char *)b, n, 0) < 0) l->send_fails++;
    }
}

/* Hold records through the reorder window before checking collisions. */
static void net_release(Link *l, int flush, uint64_t now)
{
    for (;;) {
        int best = -1;
        for (int i = 0; i < l->pend_n; i++) {
            uint64_t t = rec_t(l->pend[i]);
            if (!flush && rec_due(l, l->pend[i]) > (int64_t)now) continue;
            if (best < 0 || t < rec_t(l->pend[best])) best = i;
        }
        if (best < 0) return;

        uint8_t *r = l->pend[best];
        uint32_t id = rec_id(r);
        uint64_t t0 = rec_t(r), t1 = t0 + rec_dur(r);
        uint8_t  b  = rec_byte(r);

        /* Corrupt every overlapping transmission so its checksum fails. */
        if (coll_overlaps(l, id, t0, t1, best)) {
            uint8_t was = b;
            b = (uint8_t)(b | 0xA5);
            if (b == was) b = (uint8_t)~was;
            if (l->collisions++ == 0)
                fprintf(stderr, "[link] collision: two devices transmitting at once\n");
        }
        coll_remember(l, id, t0, t1);
        if (t0 > l->released_t_max) l->released_t_max = t0;

        unsigned used = l->tail[0] - l->head[0];
        if (used < LINK_FIFO_N) {
            l->rxq[0][l->tail[0] % LINK_FIFO_N] = b;
            l->tail[0]++;
            l->bytes_ba++;
            /* Backlog measures byte-times behind the wire. */
            if (used + 1 > l->backlog_max) l->backlog_max = used + 1;
        } else if (l->overflows++ == 0) {
            fprintf(stderr, "[link] WARNING: FIFO overflow - link bytes are being dropped\n");
        }
        l->pend_n--;
        if (best != l->pend_n) memcpy(l->pend[best], l->pend[l->pend_n], LINK_REC_N);
    }
}

/* Hubs relay complete records with their original sender and timestamp. */
static void net_pump(Link *l, uint64_t now);

/* NFC record type 1 continues a frame; type 2 ends it. */
#define NFC_REC_MORE 1
#define NFC_REC_LAST 2

static void nfc_rec_in(Link *l, int i, const uint8_t *r)
{
    int max = (int)sizeof l->nfc_asm[0];
    if (l->nfc_asm_n[i] < max) l->nfc_asm[i][l->nfc_asm_n[i]++] = r[4];
    if (r[5] != NFC_REC_LAST) return;
    int len = l->nfc_asm_n[i];
    l->nfc_asm_n[i] = 0;
    if (len <= 0) return;
    if (l->nfc_q_tail - l->nfc_q_head >= LINK_NFC_Q) {
        l->nfc_frames_dropped++;
        return;
    }
    unsigned slot = l->nfc_q_tail++ % LINK_NFC_Q;
    memcpy(l->nfc_q[slot], l->nfc_asm[i], (size_t)len);
    l->nfc_q_len[slot] = len;
    l->nfc_frames_rx++;
}

void link_nfc_tx(Link *l, const uint8_t *frame, int len)
{
    if (!l || !l->net || len <= 0) return;
    for (int k = 0; k < len; k++) {
        uint8_t rec[LINK_REC_N];
        rec_put(rec, l->my_id, frame[k], 0, link_now_us());
        rec[5] = (k == len - 1) ? NFC_REC_LAST : NFC_REC_MORE;
        net_send_all(l, -1, rec, LINK_REC_N);
    }
    l->nfc_frames_tx++;
}

static uint64_t clock_us(Link *l)
{
    return l->test_now_set ? l->test_now : link_now_us();
}

int link_nfc_rx(Link *l, uint8_t *out, int max)
{
    if (!l || !l->net) return 0;
    net_pump(l, clock_us(l));
    if (l->nfc_q_head == l->nfc_q_tail) return 0;
    unsigned slot = l->nfc_q_head++ % LINK_NFC_Q;
    int len = l->nfc_q_len[slot];
    if (len > max) len = max;
    memcpy(out, l->nfc_q[slot], (size_t)len);
    return len;
}

static void net_pump(Link *l, uint64_t now)
{
    uint8_t buf[LINK_REC_N * 64];
    net_accept_pending(l);
    for (int i = 0; i < l->nsock; i++) {
        for (;;) {
            if (l->pend_n > LINK_PEND_MAX - 64) break;
            int want = LINK_REC_N * 64 - l->part_n[i];
            int n = (int)recv((int)l->peer[i], (char *)buf, want, 0);
            if (n == 0) { net_drop_sock(l, i); i--; break; }
            if (n < 0) break;
            /* Complete any record split across socket reads. */
            for (int k = 0; k < n; k++) {
                l->part[i][l->part_n[i]++] = buf[k];
                if (l->part_n[i] < LINK_REC_N) continue;
                l->part_n[i] = 0;
                if (l->hub) net_send_all(l, i, l->part[i], LINK_REC_N);
                /* NFC frames bypass the IR timing and collision model. */
                if (l->part[i][5] != 0) { nfc_rec_in(l, i, l->part[i]); continue; }
                org_sample(l, rec_id(l->part[i]), rec_t(l->part[i]), now);
                if (rec_t(l->part[i]) < l->released_t_max) l->reorder_late++;
                if (l->pend_n < LINK_PEND_MAX)
                    memcpy(l->pend[l->pend_n++], l->part[i], LINK_REC_N);
                if (l->pend_n > l->pend_max) l->pend_max = l->pend_n;
            }
        }
    }
    net_release(l, 0, now);
}

int link_host(Link *l, int port)
{
    wsa_init();
    uintptr_t ls = (uintptr_t)socket(AF_INET, SOCK_STREAM, 0);
    if (ls == (uintptr_t)INVALID_SOCKET) return 0;
    int one = 1;
    setsockopt((int)ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind((int)ls, (struct sockaddr *)&a, sizeof a) != 0) { CLOSESOCK((int)ls); return 0; }
    if (listen((int)ls, LINK_PEERS_MAX) != 0) { CLOSESOCK((int)ls); return 0; }
    fprintf(stderr, "[link] hosting on 127.0.0.1:%d - waiting for the other emulator...\n", port);
    uintptr_t cs = (uintptr_t)accept((int)ls, NULL, NULL);
    if (cs == (uintptr_t)INVALID_SOCKET) { CLOSESOCK((int)ls); return 0; }
    net_add_sock(l, cs);
    /* Keep listening so more devices can join the hub. */
    sock_setup(ls);
    l->listener = ls; l->listening = 1; l->hub = 1;
    fprintf(stderr, "[link] peer connected. Still listening on %d for a third.\n", port);
    return 1;
}

static int try_join(Link *l, const char *host, int port, int quiet)
{
    wsa_init();
    uintptr_t s = (uintptr_t)socket(AF_INET, SOCK_STREAM, 0);
    if (s == (uintptr_t)INVALID_SOCKET) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = inet_addr(host);
    if (!quiet) fprintf(stderr, "[link] connecting to %s:%d ...\n", host, port);
    if (connect((int)s, (struct sockaddr *)&a, sizeof a) != 0) {
        if (!quiet)
            fprintf(stderr, "[link] could not connect to %s:%d (is the host running?)\n",
                    host, port);
        CLOSESOCK((int)s);
        return 0;
    }
    net_add_sock(l, s);
    fprintf(stderr, "[link] connected to peer at %s:%d.\n", host, port);
    return 1;
}

/* Accepts "host:port", a bare port, or a bare host. */
static void parse_hostport(const char *s, char *host, size_t hostsz, int *port)
{
    snprintf(host, hostsz, "127.0.0.1");
    *port = 7878;
    if (!s || !*s) return;
    const char *colon = strrchr(s, ':');
    if (colon) {
        size_t n = (size_t)(colon - s);
        if (n && n < hostsz) { memcpy(host, s, n); host[n] = 0; }
        *port = atoi(colon + 1);
    } else if (s[0] >= '0' && s[0] <= '9' && !strchr(s, '.')) {
        *port = atoi(s);
    } else {
        snprintf(host, hostsz, "%s", s);
    }
}

int link_join(Link *l, const char *hostport)
{
    char host[128]; int port;
    parse_hostport(hostport, host, sizeof host, &port);
    return try_join(l, host, port, 0);
}

/* The first peer hosts; later peers join it. */
int link_peer(Link *l, int port)
{
    if (try_join(l, "127.0.0.1", port, 1)) return 1;
    fprintf(stderr, "[link] no peer on port %d yet - hosting instead.\n", port);
    return link_host(l, port);
}

/* Auto-link joins an existing session or leaves a non-blocking listener open. */
int link_auto_begin(Link *l, int port)
{
    if (try_join(l, "127.0.0.1", port, 1)) return 1;
    wsa_init();
    uintptr_t ls = (uintptr_t)socket(AF_INET, SOCK_STREAM, 0);
    if (ls == (uintptr_t)INVALID_SOCKET) return 0;
    int one = 1;
    setsockopt((int)ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* never expose the listener remotely */
    a.sin_port = htons((unsigned short)port);
    if (bind((int)ls, (struct sockaddr *)&a, sizeof a) != 0
        || listen((int)ls, LINK_PEERS_MAX) != 0) {
        CLOSESOCK((int)ls);                        /* another listener won the race */
        return 0;
    }
    sock_setup(ls);
    l->listener = ls; l->listening = 1; l->hub = 1;
    fprintf(stderr, "[link] connect mode - listening on 127.0.0.1:%d for another device\n", port);
    return 0;
}

int link_auto_poll(Link *l)
{
    if (!l->listening) return 0;
    int before = l->nsock;
    net_accept_pending(l);
    if (l->nsock == before) return 0;
    fprintf(stderr, "[link] the other device connected.\n");
    return 1;
}

void link_close(Link *l)
{
    if (l->listening) { CLOSESOCK((int)l->listener); l->listening = 0; }
    for (int i = 0; i < l->nsock; i++) CLOSESOCK((int)l->peer[i]);
    l->nsock = 0; l->net = 0; l->hub = 0;
}

static void push_to(Link *l, int to, uint8_t b)
{
    unsigned *head = &l->head[to], *tail = &l->tail[to];
    if (*tail - *head >= LINK_FIFO_N) {
        if ((l->overflows)++ == 0)
            fprintf(stderr, "[link] WARNING: FIFO overflow - link bytes are being dropped\n");
        return;
    }
    l->rxq[to][*tail % LINK_FIFO_N] = b;
    (*tail)++;
}

static int pop_from(Link *l, int from)
{
    unsigned *head = &l->head[from], *tail = &l->tail[from];
    if (*tail == *head) return -1;
    uint8_t b = l->rxq[from][*head % LINK_FIFO_N];
    (*head)++;
    return (int)b;
}

static int npeers_clamped(Link *l)
{
    /* An unset peer count still means a two-device bus. */
    if (l->npeers < 2) return 2;
    return l->npeers > LINK_PEERS_MAX ? LINK_PEERS_MAX : l->npeers;
}

void link_reset(Link *l)
{
    for (int i = 0; i < LINK_PEERS_MAX; i++) l->head[i] = l->tail[i] = 0;
    l->npeers = 2;
    l->bytes_ab = l->bytes_ba = 0;
    l->overflows = 0;
    l->net = 0; l->listening = 0; l->hub = 0;
    l->nsock = 0; l->listener = 0;
    for (int i = 0; i < LINK_PEERS_MAX; i++) { l->peer[i] = 0; l->part_n[i] = 0; }
    /* Process-local IDs distinguish collision sources without a handshake. */
    l->my_id = (uint32_t)(link_now_us() * 2654435761u) ^ (uint32_t)(uintptr_t)l;
    if (!l->my_id) l->my_id = 1;
    l->tx_byte_us = 0;
    l->seen_n = 0; l->pend_n = 0; l->collisions = 0;
    l->backlog_max = 0; l->pend_max = 0; l->send_fails = 0;
    l->org_n = 0; l->released_t_max = 0; l->reorder_late = 0;
    l->test_now = 0; l->test_now_set = 0;
}

void link_set_tx_byte_us(Link *l, unsigned us) { l->tx_byte_us = us; }

uint64_t link_collisions(Link *l) { return l->collisions; }
unsigned link_backlog_max(Link *l) { return l->backlog_max; }
int      link_pend_max(Link *l)    { return l->pend_max; }
uint64_t link_send_fails(Link *l)  { return l->send_fails; }
uint64_t link_reorder_late(Link *l) { return l->reorder_late; }

void link_test_inject(Link *l, uint32_t id, uint8_t byte, uint64_t t_us, unsigned dur_us)
{
    if (l->pend_n >= LINK_PEND_MAX) return;
    org_sample(l, id, t_us, clock_us(l));
    if (t_us < l->released_t_max) l->reorder_late++;
    rec_put(l->pend[l->pend_n++], id, byte, (uint16_t)dur_us, t_us);
}

void link_test_release(Link *l) { net_release(l, 1, clock_us(l)); }

void link_test_now(Link *l, uint64_t now_us) { l->test_now = now_us; l->test_now_set = 1; }

int64_t link_test_offset(Link *l, uint32_t id)
{
    int i = org_find(l, id);
    return i < 0 ? INT64_MAX : l->org_off[i];
}

void link_tx(Link *l, int from_core, uint8_t byte)
{
    if (l->net) {
        /* Sender and wire time let peers detect overlapping transmissions. */
        uint8_t rec[LINK_REC_N];
        unsigned dur = l->tx_byte_us ? l->tx_byte_us : 100;
        if (dur > 0xFFFF) dur = 0xFFFF;
        rec_put(rec, l->my_id, byte, (uint16_t)dur, link_now_us());
        net_send_all(l, -1, rec, LINK_REC_N);
        l->bytes_ab++;
        return;
    }
    int n = npeers_clamped(l);
    for (int to = 0; to < n; to++)
        if (to != from_core) push_to(l, to, byte);
    if (from_core == 0) l->bytes_ab++;
    else                l->bytes_ba++;
}

/* Each Link must use one monotonic clock for all releases. */
int link_rx_at(Link *l, int for_core, uint64_t now_us)
{
    /* A networked process always owns participant 0. */
    if (l->net) { net_pump(l, now_us); return pop_from(l, 0); }
    return pop_from(l, for_core);
}

int link_rx_pending_at(Link *l, int for_core, uint64_t now_us)
{
    if (l->net) { net_pump(l, now_us); return l->tail[0] != l->head[0]; }
    return l->tail[for_core] != l->head[for_core];
}

int link_rx(Link *l, int for_core)
{
    return link_rx_at(l, for_core, clock_us(l));
}

int link_rx_pending(Link *l, int for_core)
{
    return link_rx_pending_at(l, for_core, clock_us(l));
}

/* --ir-log samples execution in 16-byte buckets. */
void link_ir_pc_sample(Emu *e)
{
    /* Limit samples to active IR sessions so idle loops do not dominate. */
    if (e->a0ram[0xDB3] != 2 &&
        !(e->ir_active_at > 0.0 && e->emu_secs - e->ir_active_at < 5.0)) return;
    uint32_t pg = e->pc & 0xFFFFFFF0u;
    e->pc_samples++;
    for (int i = 0; i < e->pc_hist_n; i++)
        if (e->pc_hist[i] == pg) { e->pc_hits[i]++; return; }
    if (e->pc_hist_n < 96) {
        e->pc_hist[e->pc_hist_n] = pg;
        e->pc_hits[e->pc_hist_n] = 1;
        e->pc_hist_n++;
    }
}

void link_ir_pc_report(Emu *e)
{
    /* Idle drops are expected while the peer is still in menus. */
    if (e->ir_rx_dropped_idle)
        fprintf(stderr, "[ir%c] peer bytes dropped while not in IR mode: %llu\n",
                e->core_id ? 'B' : 'A', (unsigned long long)e->ir_rx_dropped_idle);
    /* Multi-device collisions trigger firmware retries. */
    if (e->link && link_collisions(e->link))
        fprintf(stderr, "[ir%c] bytes destroyed by collision: %llu\n",
                e->core_id ? 'B' : 'A', (unsigned long long)link_collisions(e->link));
    if (e->link && link_backlog_max(e->link) > 1)
        fprintf(stderr, "[ir%c] rx backlog high-water: %u bytes (~%.1f ms behind the "
                "wire at 87us/byte)\n", e->core_id ? 'B' : 'A',
                link_backlog_max(e->link), link_backlog_max(e->link) * 0.087);
    if (e->link && link_pend_max(e->link))
        fprintf(stderr, "[ir%c] settle queue high-water: %d of %d records\n",
                e->core_id ? 'B' : 'A', link_pend_max(e->link), LINK_PEND_MAX);
    if (e->link && link_send_fails(e->link))
        fprintf(stderr, "[ir%c] WARNING: %llu records the socket refused - those "
                "bytes never reached a peer\n", e->core_id ? 'B' : 'A',
                (unsigned long long)link_send_fails(e->link));
    /* Late records mean LINK_LEAD_US no longer covers transport jitter. */
    if (e->link && link_reorder_late(e->link))
        fprintf(stderr, "[ir%c] WARNING: %llu records arrived after a younger one "
                "was already released - LINK_LEAD_US no longer covers the "
                "transport's reorder jitter\n", e->core_id ? 'B' : 'A',
                (unsigned long long)link_reorder_late(e->link));
    if (!e->pc_samples) return;
    fprintf(stderr, "[ir%c] where core spent its time (%llu samples):\n",
            e->core_id ? 'B' : 'A', (unsigned long long)e->pc_samples);
    for (int k = 0; k < 8; k++) {
        int best = -1; uint32_t bh = 0;
        for (int i = 0; i < e->pc_hist_n; i++)
            if (e->pc_hits[i] > bh) { bh = e->pc_hits[i]; best = i; }
        if (best < 0 || !bh) break;
        fprintf(stderr, "[ir%c]   %08x  %5.1f%%  %s\n", e->core_id ? 'B' : 'A',
                e->pc_hist[best], 100.0 * bh / (double)e->pc_samples,
                disasm_sym_for(e->pc_hist[best]));
        e->pc_hits[best] = 0;
    }
}

void link_core_step(Emu *e, uint8_t btn_mask)
{
    if (e->stopped) return;
    cpu_step(e);
    if (e->cycles - e->last_tick >= 256) {
        periph_tick(e, (uint32_t)(e->cycles - e->last_tick));
        e->last_tick = e->cycles;
        periph_buttons(e, btn_mask);
    }
}

void link_lockstep_run(Emu *a, Emu *b, uint64_t quantum, uint8_t amask, uint8_t bmask)
{
    uint64_t at = a->cycles + quantum;
    while (a->cycles < at && !a->stopped) link_core_step(a, amask);
    uint64_t bt = b->cycles + quantum;
    while (b->cycles < bt && !b->stopped) link_core_step(b, bmask);
}

/* Returns the focused core, or -1 for a non-button key. */
int link_route_key(int focus, int sdl_key_bit)
{
    if (sdl_key_bit == 0) return -1;
    return focus & 1;
}
