/* --nfc-inject models a phone target; pn512.c adds the air CRC. */
#include "emu.h"
#include "nfc_taps.h"
#include <stdlib.h>
#include <string.h>

enum {
    NP_WAIT_ATR = 0,
    NP_LINK,
    NP_CONNECTING,
    NP_SENDING,
    NP_RECEIVING,
    NP_TAP_DONE,
};

enum { LL_SYMM = 0, LL_CONNECT = 4, LL_DISC = 5, LL_CC = 6, LL_DM = 7,
       LL_I = 12, LL_RR = 13 };

#define LL_OUR_SAP  32
#define LL_SNEP_SAP 4           /* well-known SNEP service */
#define LL_FRAG     125         /* MIU 128 minus the LLCP I header */

/* Replies must land inside the initiator's receive window. */
#define NP_REPLY_US 300.0

static void np_log_frame(Emu *e, const char *dir, const uint8_t *f, int n)
{
    NfcPeer *p = e->nfc_vpeer;
    if (!p->log) return;
    fprintf(stderr, "[phone] %s %2d:", dir, n);
    for (int i = 0; i < n && i < 24; i++) fprintf(stderr, " %02x", f[i]);
    fprintf(stderr, "%s\n", n > 24 ? " ..." : "");
}

/* DEP is request/response, so only one reply may be pending. */
static void np_reply_ex(Emu *e, const uint8_t *f, int n, int crc)
{
    NfcPeer *p = e->nfc_vpeer;
    double mhz = e->cmu.mclk_hz > 0 ? e->cmu.mclk_hz : e->dev.osc3_hz;
    if (p->out_at)
        fprintf(stderr, "[phone] WARNING: reply queued while one was pending\n");
    if (n > (int)sizeof p->out) {
        fprintf(stderr, "[phone] reply too long (%d bytes), dropped\n", n);
        return;
    }
    memcpy(p->out, f, (size_t)n);
    p->out_n = n;
    p->out_crc = crc;
    p->out_at = e->cycles + (uint64_t)(mhz * 1e-6 * NP_REPLY_US);
    np_log_frame(e, "TX", f, n);
}

static void np_reply(Emu *e, const uint8_t *f, int n)
{
    np_reply_ex(e, f, n, 1);
}

/* DEP_RES echoes PNI; DID, NAD, and chaining are unused. */
static void np_send_llcp(Emu *e, const uint8_t *pdu, int n)
{
    NfcPeer *p = e->nfc_vpeer;
    uint8_t f[PN512_AIR_N];
    if (n + 4 > (int)sizeof f) {
        fprintf(stderr, "[phone] LLCP PDU too long (%d)\n", n);
        return;
    }
    f[0] = (uint8_t)(n + 4);
    f[1] = 0xD5; f[2] = 0x07;
    f[3] = (uint8_t)(p->pni & 3);
    memcpy(f + 4, pdu, (size_t)n);
    np_reply(e, f, n + 4);
}

/* LLCP header: DSAP[15:10] | PTYPE[9:6] | SSAP[5:0], big endian. */
static int np_llcp_hdr(uint8_t *o, int dsap, int ptype, int ssap)
{
    unsigned v = ((unsigned)(dsap & 0x3F) << 10) |
                 ((unsigned)(ptype & 0x0F) << 6) | (unsigned)(ssap & 0x3F);
    o[0] = (uint8_t)(v >> 8); o[1] = (uint8_t)v;
    return 2;
}

static void np_send_symm(Emu *e)
{
    uint8_t pdu[2];
    np_llcp_hdr(pdu, 0, LL_SYMM, 0);
    np_send_llcp(e, pdu, 2);
}

/* Build the tap payload, then wrap it in NDEF and SNEP. */
static void np_build_tap(Emu *e)
{
    NfcPeer *p = e->nfc_vpeer;
    uint8_t app[NFCPEER_MAX];
    int total;

    if (p->tap == 0) {
        /* Restore six omitted zero bytes so the handshake matches its size. */
        memset(app, 0, 258);
        memcpy(app, nfc_tap_handshake, 242);
        memcpy(app + 248, nfc_tap_handshake + 242, 10);
        total = 258;
    } else {
        int item = p->payload_n;
        total = 256 + 8 + item + 2;
        if (total > (int)sizeof app) {
            fprintf(stderr, "[phone] item too large for one tap (%d bytes)\n", item);
            p->state = NP_TAP_DONE;
            return;
        }
        memset(app, 0, (size_t)total);
        memcpy(app, nfc_tap_download, 256);
        app[256] = 0x03; app[257] = 0; app[258] = 0; app[259] = 0;
        app[260] = 0x01; app[261] = 0;
        app[262] = (uint8_t)item; app[263] = (uint8_t)(item >> 8);
        memcpy(app + 264, p->payload, (size_t)item);
        app[72] = (uint8_t)total; app[73] = (uint8_t)(total >> 8);
        app[132] = 0x01;                            /* normal download */
        unsigned sum = 0;
        for (int i = 0; i < total - 2; i++) sum = (sum + app[i]) & 0xFFFF;
        app[total - 2] = (uint8_t)sum;
        app[total - 1] = (uint8_t)(sum >> 8);
    }
    while (total & 3) app[total++] = 0;             /* 4-byte alignment */

    /* One long-form NDEF MIME record (TNF 2, MB|ME). */
    static const char mime[] = "application/jp.co.bandai.tamagotchiapp";
    int tl = (int)sizeof mime - 1;
    uint8_t ndef[NFCPEER_MAX];
    int n = 0;
    ndef[n++] = 0xC2;
    ndef[n++] = (uint8_t)tl;
    ndef[n++] = (uint8_t)(total >> 24); ndef[n++] = (uint8_t)(total >> 16);
    ndef[n++] = (uint8_t)(total >> 8);  ndef[n++] = (uint8_t)total;
    memcpy(ndef + n, mime, (size_t)tl); n += tl;
    memcpy(ndef + n, app, (size_t)total); n += total;

    /* SNEP PUT: version 1.0, code 0x02, big-endian length. */
    p->tx_n = 0;
    p->tx[p->tx_n++] = 0x10;
    p->tx[p->tx_n++] = 0x02;
    p->tx[p->tx_n++] = (uint8_t)(n >> 24); p->tx[p->tx_n++] = (uint8_t)(n >> 16);
    p->tx[p->tx_n++] = (uint8_t)(n >> 8);  p->tx[p->tx_n++] = (uint8_t)n;
    memcpy(p->tx + p->tx_n, ndef, (size_t)n);
    p->tx_n += n;
    p->tx_sent = 0;
    if (p->log)
        fprintf(stderr, "[phone] tap %d: app %d bytes -> NDEF %d -> SNEP %d "
                        "(%d fragments)\n", p->tap + 1, total, n, p->tx_n,
                (p->tx_n + LL_FRAG - 1) / LL_FRAG);
}

static void np_send_fragment(Emu *e)
{
    NfcPeer *p = e->nfc_vpeer;
    int left = p->tx_n - p->tx_sent;
    int n = left > LL_FRAG ? LL_FRAG : left;
    uint8_t pdu[3 + LL_FRAG];
    int k = np_llcp_hdr(pdu, LL_SNEP_SAP, LL_I, LL_OUR_SAP);
    pdu[k++] = (uint8_t)(((p->ns & 15) << 4) | (p->nr & 15));
    memcpy(pdu + k, p->tx + p->tx_sent, (size_t)n);
    p->tx_sent += n;
    p->ns = (uint8_t)((p->ns + 1) & 15);
    np_send_llcp(e, pdu, k + n);
}

/* ATR_RES layout: LEN D5 01 NFCID3t[10] DIDt BSt BRt TO PPt Gt[20].
 * LEN includes itself; Gt contains the LLCP parameters. */
static void np_send_atr_res(Emu *e, const uint8_t *atr_req, int req_len)
{
    NfcPeer *p = e->nfc_vpeer;
    static const uint8_t gt[20] = {
        0x46,0x66,0x6D,             /* "Ffm" */
        0x01,0x01,0x13,             /* VERSION 1.3 */
        0x02,0x02,0x07,0xFF,        /* MIUX -> 2175 */
        0x03,0x02,0x00,0x13,        /* WKS: SAPs 0, 1, 4 */
        0x04,0x01,0x96,             /* LTO 1500 ms */
        0x07,0x01,0x03,             /* OPT: link service class 3 */
    };
    uint8_t f[64];
    int n = 0;
    /* PPi and "Ffm" distinguish LLCP from raw DEP. */
    const uint8_t *b = atr_req + 1;
    int bn = req_len - 1;
    p->did = (bn >= 13) ? b[12] : 0;              /* DIDi, echoed back */
    p->llcp = 0;
    if (bn >= 16 && (b[15] & 0x02) && bn >= 19)
        p->llcp = (b[16] == 0x46 && b[17] == 0x66 && b[18] == 0x6D);
    if (p->log)
        fprintf(stderr, "[phone] ATR_REQ: DID %02x, PPi %02x -> %s peer\n",
                p->did, bn >= 16 ? b[15] : 0, p->llcp ? "LLCP" : "raw-DEP");
    f[n++] = 0;
    f[n++] = 0xD5; f[n++] = 0x01;
    memcpy(f + n, p->nfcid3, 10); n += 10;
    f[n++] = p->did;
    f[n++] = 0x00;                 /* BSt */
    f[n++] = 0x00;                 /* BRt */
    f[n++] = 0x08;                 /* TO */
    f[n++] = 0x32;                 /* PPt */
    memcpy(f + n, gt, sizeof gt); n += (int)sizeof gt;
    f[0] = (uint8_t)n;
    np_reply(e, f, n);

    /* A new ATR_REQ is a new tap: reset the link, keep the tap counter. */
    p->state = NP_LINK;
    p->ns = p->nr = 0;
    p->rx_n = p->rx_want = 0;
    p->tx_n = p->tx_sent = 0;
    p->ll_peer_sap = 0;
}

static void np_llcp_in(Emu *e, const uint8_t *pdu, int n)
{
    NfcPeer *p = e->nfc_vpeer;
    if (n < 2) { np_send_symm(e); return; }
    unsigned v = ((unsigned)pdu[0] << 8) | pdu[1];
    int dsap  = (int)((v >> 10) & 0x3F);
    int ptype = (int)((v >> 6) & 0x0F);
    int ssap  = (int)(v & 0x3F);
    const uint8_t *body = pdu + 2;
    int bn = n - 2;

    if (ptype == LL_DISC) {
        uint8_t o[3];
        int k = np_llcp_hdr(o, ssap, LL_DM, dsap);
        o[k++] = 0x00;
        np_send_llcp(e, o, k);
        p->state = NP_TAP_DONE;
        return;
    }

    /* The tama uses this connection for its ID reply or download confirmation. */
    if (ptype == LL_CONNECT && dsap == LL_SNEP_SAP) {
        p->ll_peer_sap = (uint8_t)ssap;
        p->nr = 0;
        p->rx_n = p->rx_want = 0;
        uint8_t o[16];
        int k = np_llcp_hdr(o, ssap, LL_CC, LL_SNEP_SAP);
        o[k++] = 0x02; o[k++] = 0x02; o[k++] = 0x07; o[k++] = 0x40;  /* MIUX */
        o[k++] = 0x05; o[k++] = 0x01; o[k++] = 0x0F;                 /* RW 15 */
        np_send_llcp(e, o, k);
        p->state = NP_RECEIVING;
        return;
    }

    if (ptype == LL_I) {
        if (bn < 1) { np_send_symm(e); return; }
        int ns = (body[0] >> 4) & 15;
        const uint8_t *data = body + 1;
        int dn = bn - 1;
        p->nr = (uint8_t)((ns + 1) & 15);

        if (p->state == NP_RECEIVING || p->ll_peer_sap) {
            /* Receive a possibly fragmented SNEP PUT. */
            if (p->rx_want == 0 && dn >= 6) {
                p->rx_want = (int)(((unsigned)data[2] << 24) |
                                   ((unsigned)data[3] << 16) |
                                   ((unsigned)data[4] << 8) | data[5]);
                data += 6; dn -= 6;
            }
            if (dn > 0 && p->rx_n + dn <= (int)sizeof p->rx) {
                memcpy(p->rx + p->rx_n, data, (size_t)dn);
                p->rx_n += dn;
            }
            uint8_t o[16];
            int k;
            if (p->rx_n >= p->rx_want) {       /* complete: SNEP SUCCESS */
                k = np_llcp_hdr(o, p->ll_peer_sap, LL_I, LL_SNEP_SAP);
                o[k++] = (uint8_t)(((p->ns & 15) << 4) | (p->nr & 15));
                o[k++] = 0x10; o[k++] = 0x81;
                o[k++] = 0; o[k++] = 0; o[k++] = 0; o[k++] = 0;
                p->ns = (uint8_t)((p->ns + 1) & 15);
                np_send_llcp(e, o, k);
                if (p->log) {
                    /* Skip the NDEF header and MIME type to reach the payload. */
                    int hdr = 0;
                    if (p->rx_n > 2 && p->rx[0] == 0xC2) hdr = 6 + p->rx[1];
                    else if (p->rx_n > 2 && p->rx[0] == 0xD2) hdr = 3 + p->rx[1];
                    const uint8_t *ap = p->rx + hdr;
                    int an = p->rx_n - hdr;
                    fprintf(stderr, "[phone] tap %d reply, %d bytes payload:",
                            p->tap + 1, an);
                    for (int i = 0; i < an && i < 12; i++)
                        fprintf(stderr, " %02x", ap[i]);
                    fprintf(stderr, "%s\n", an > 12 ? " ..." : "");
                    /* Tap 1 returns the tama's identity at fixed offsets. */
                    if (p->tap == 0 && an >= 140) {
                        fprintf(stderr, "[phone] device id ");
                        for (int i = 96; i < 112; i += 2)
                            fprintf(stderr, "%s%02X%02X", i > 96 ? "-" : "",
                                    ap[i], ap[i + 1]);
                        fprintf(stderr, ", birth %02u%02u\n", ap[138], ap[139]);
                    }
                }
                p->state = NP_TAP_DONE;
                if (p->tap == 0) p->tap = 1;   /* next session sends the item */
            } else if (p->rx_n == dn) {        /* first of several: CONTINUE */
                k = np_llcp_hdr(o, p->ll_peer_sap, LL_I, LL_SNEP_SAP);
                o[k++] = (uint8_t)(((p->ns & 15) << 4) | (p->nr & 15));
                o[k++] = 0x10; o[k++] = 0x80;
                o[k++] = 0; o[k++] = 0; o[k++] = 0; o[k++] = 0;
                p->ns = (uint8_t)((p->ns + 1) & 15);
                np_send_llcp(e, o, k);
            } else {                           /* middle fragment: plain RR */
                k = np_llcp_hdr(o, p->ll_peer_sap, LL_RR, LL_SNEP_SAP);
                o[k++] = (uint8_t)(p->nr & 15);
                np_send_llcp(e, o, k);
            }
            return;
        }

        /* Handle the tama's response to our PUT. */
        if (dn >= 2 && data[1] == 0x80) {      /* CONTINUE: keep going */
            np_send_fragment(e);
        } else if (dn >= 2 && data[1] == 0x81) {   /* SUCCESS: message taken */
            uint8_t o[3];
            int k = np_llcp_hdr(o, LL_SNEP_SAP, LL_RR, LL_OUR_SAP);
            o[k++] = (uint8_t)(p->nr & 15);
            np_send_llcp(e, o, k);
            if (p->log) fprintf(stderr, "[phone] tap %d accepted by the tama\n",
                                p->tap + 1);
            p->state = NP_TAP_DONE;
        } else {
            np_send_symm(e);
        }
        return;
    }

    if (ptype == LL_CC) {
        np_build_tap(e);
        if (p->tx_n) { p->state = NP_SENDING; np_send_fragment(e); }
        else         np_send_symm(e);
        return;
    }

    if (ptype == LL_RR) {
        if (p->state == NP_SENDING && p->tx_sent < p->tx_n) np_send_fragment(e);
        else np_send_symm(e);
        return;
    }

    if (ptype == LL_SYMM) {
        /* The link is idle and it is our turn. Open the SNEP connection the
         * first time; keep pushing if a message is half sent. */
        if (p->state == NP_LINK) {
            uint8_t o[2];
            np_llcp_hdr(o, LL_SNEP_SAP, LL_CONNECT, LL_OUR_SAP);
            np_send_llcp(e, o, 2);
            p->state = NP_CONNECTING;
        } else if (p->state == NP_SENDING && p->tx_sent < p->tx_n) {
            np_send_fragment(e);
        } else {
            np_send_symm(e);
        }
        return;
    }

    np_send_symm(e);
}

void nfcpeer_rf_in(Emu *e, const uint8_t *frame, int len)
{
    NfcPeer *p = e->nfc_vpeer;
    if (!p || len < 1) return;
    p->frames_in++;
    np_log_frame(e, "RX", frame, len);

    /* Activate as a passive Type-A target before starting NFC-DEP. Activation
     * responses are bare; SEL_RES 0x40 advertises NFC-DEP support. */
    if (len == 1 && (frame[0] == 0x26 || frame[0] == 0x52)) {   /* REQA / WUPA */
        static const uint8_t atqa[2] = {0x01, 0x01};
        p->activated = 0;
        np_reply_ex(e, atqa, 2, 0);
        return;
    }
    if (len == 2 && frame[0] == 0x93 && frame[1] == 0x20) {     /* anticollision */
        uint8_t a[5];
        memcpy(a, p->uid, 4);
        a[4] = (uint8_t)(a[0] ^ a[1] ^ a[2] ^ a[3]);            /* BCC */
        np_reply_ex(e, a, 5, 0);
        return;
    }
    if (len >= 7 && frame[0] == 0x93 && frame[1] == 0x70) {     /* SELECT */
        uint8_t sak[1] = {0x40};                 /* bit6: NFC-DEP supported */
        p->activated = 1;
        np_reply(e, sak, 1);                     /* SAK does carry a CRC */
        return;
    }

    const uint8_t *b = frame + 1;              /* past LEN */
    int bn = len - 1;
    if (bn < 2) return;

    if (b[0] == 0xD4 && b[1] == 0x00) {        /* ATR_REQ */
        np_send_atr_res(e, frame, len);
        return;
    }
    if (b[0] == 0xD4 && b[1] == 0x06) {        /* DEP_REQ */
        if (bn < 3) return;
        uint8_t pfb = b[2];
        p->pni = (uint8_t)(pfb & 0x03);
        int has_did = (pfb >> 2) & 1;
        int has_nad = (pfb >> 3) & 1;
        int off = 3 + has_did + has_nad;
        if (off > bn) return;
        const uint8_t *data = b + off;
        int dn = bn - off;

        if (p->llcp) { np_llcp_in(e, data, dn); return; }
        /* Raw greeting records receive a bare DEP_RES. */
        uint8_t f[4] = { 4, 0xD5, 0x07, p->pni };
        np_reply(e, f, 4);
        return;
    }
}

/* Deliver a queued reply at its scheduled cycle. */
void nfcpeer_tick(Emu *e)
{
    NfcPeer *p = e->nfc_vpeer;
    if (!p->out_at || e->cycles < p->out_at) return;
    p->out_at = 0;
    p->frames_out++;
    pn512_rf_deliver_ex(e, p->out, p->out_n, p->out_crc);
}

int nfcpeer_open(Emu *e, const char *payload_path)
{
    FILE *f = fopen(payload_path, "rb");
    if (!f) { fprintf(stderr, "[phone] cannot open %s\n", payload_path); return 0; }
    NfcPeer *p = calloc(1, sizeof *p);
    if (!p) { fclose(f); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > NFCPEER_MAX - 512) {
        fprintf(stderr, "[phone] %s: %ld bytes is not a usable item payload\n",
                payload_path, sz);
        fclose(f); free(p); return 0;
    }
    p->payload = malloc((size_t)sz);
    if (!p->payload || fread(p->payload, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "[phone] %s: short read\n", payload_path);
        fclose(f); free(p->payload); free(p); return 0;
    }
    fclose(f);
    p->payload_n = (int)sz;
    /* NFCID3t must carry the 4U F0 40 signature or the tama keeps polling. */
    static const uint8_t id[10] = {0xF0,0x40,0x5A,0x11,0x93,0x2C,0x7E,0x04,0xF0,0x40};
    memcpy(p->nfcid3, id, sizeof id);
    /* NFCID1t starts with the PN532's random, non-unique 0x08 prefix. */
    p->uid[0] = 0x08; p->uid[1] = 0x5A; p->uid[2] = 0x11; p->uid[3] = 0x93;
    p->log = e->nfc_log;
    e->nfc_vpeer = p;
    fprintf(stderr, "[phone] item payload %s (%d bytes)%s\n", payload_path,
            p->payload_n,
            memcmp(p->payload, "TAMAGO", 6) == 0 ? ", TAMAGO header ok"
                                                 : " - WARNING: no TAMAGO header");
    return 1;
}

void nfcpeer_report(Emu *e, FILE *f)
{
    NfcPeer *p = e->nfc_vpeer;
    if (!p) return;
    fprintf(f, "[phone] frames heard %llu, sent %llu; reached tap %d, phase %d\n",
            (unsigned long long)p->frames_in, (unsigned long long)p->frames_out,
            p->tap + 1, p->state);
}
