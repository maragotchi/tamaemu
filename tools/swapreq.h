/* src/main.c parses this protocol independently; keep both sides in sync.
 * Layout, little-endian:
 *   0  magic "TAMASWAP" (8 bytes)
 *   8  uint32 version = 1
 *  12  uint32 count
 *  16  uint32 sum      - sum32 of everything after this field
 *  20  count entries of: uint32 off, uint32 len, uint32 fill
 *      then, for each entry with fill == 0, len bytes of payload in order
 *
 * fill == 1 means "write len bytes of 0xFF at off" and carries no payload,
 * which is how a slot is freed without putting 32 KB of 0xFF in the file.
 */
#ifndef SWAPREQ_H
#define SWAPREQ_H
#include <stdint.h>
#include <stddef.h>

#define SWAPREQ_MAGIC   "TAMASWAP"
#define SWAPREQ_VERSION 1u
#define SWAPREQ_MAX     8          /* one replacement uses two entries */

typedef struct {
    uint32_t off;                  /* flash-image offset */
    uint32_t len;
    const uint8_t *bytes;          /* NULL = fill with 0xFF */
} SwapWrite;

typedef struct {
    int count;
    SwapWrite w[SWAPREQ_MAX];
    uint8_t *owned;                /* freed by swapreq_free() */
} SwapReq;

/* Write through path.tmp so readers never see partial data. */
int  swapreq_write(const char *path, const SwapWrite *w, int count);

/* Return 0 and fill r, or -1 for an invalid or unreadable request. Call
 * swapreq_free() after a successful read. */
int  swapreq_read(const char *path, SwapReq *r);
void swapreq_free(SwapReq *r);

#endif
