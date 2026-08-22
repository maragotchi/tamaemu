/* Install downloads into device-specific fixed-size flash slots. */
#ifndef DLC_H
#define DLC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DLC_IMAGE_SIZE (8u * 1024u * 1024u)
#define DLC_PAYLOAD_MAX (64u * 1024u * 1024u)
#define DLC_SCAN_MAX_ENTRIES 6767
#define DLC_ID_MAX     0x15      /* header id field is 0x34..0x48 + NUL */
#define DLC_NAME_MAX   256       /* UTF-8 of a 23-unit UTF-16BE name */
#define DLC_ERR_MAX    160

typedef struct {
    int      kind;
    uint32_t base;
    uint32_t slotsz;
    int      nslots;      /* firmware limit */
    const char *label;
} DlcKind;

/* Result of installing one payload. */
typedef struct {
    char        file[260];
    int         type;                  /* -1 when unknown */
    char        id[DLC_ID_MAX];
    char        name[DLC_NAME_MAX];
    const char *label;                 /* NULL when unroutable */
    int         kind;                  /* -1 when unroutable */
    long        off;                   /* -1 when not placed */
    char        error[DLC_ERR_MAX];    /* "" on success */
} DlcResult;

typedef struct { const char *label; int used, max; } DlcUsage;

/* One store slot; records are stored verbatim. */
typedef struct {
    int  slot;                 /* index within the store */
    int  occupied;             /* first six bytes are "TAMAGO" */
    long off;                  /* image offset of this slot */
    char id[DLC_ID_MAX];       /* "" when empty */
    char name[DLC_NAME_MAX];   /* "" when empty */
} DlcSlot;

/* Route a record to a store. kind -1 means no known destination; label may
 * explain recognized content. rec/reclen may be NULL/0 for header-only calls. */
typedef void (*DlcRouteFn)(int typ, int cat, const char *ascii_id, int flag,
                           const uint8_t *rec, size_t reclen,
                           const char **label, int *kind);

/* One device's content layout. nkinds == 0 disables installation. */
typedef struct {
    const char *name;         /* --device value */
    const char *title;        /* user-facing name */
    const char *shortname;    /* non-empty launcher tab label */
    /* First vector-table word, little-endian; 0 disables signature matching. */
    uint32_t reset_vec;
    const DlcKind *kinds;     /* NULL when the stores are not known */
    int nkinds;
    const int *wipe;          /* kinds Wipe may clear */
    int nwipe;
    const char *const *tabs;  /* library tabs, in display order */
    int ntabs;
    DlcRouteFn route;         /* NULL when nothing routes yet */

    /* Bytes; 0 selects DLC_IMAGE_SIZE. */
    uint32_t image_size;

    /* Header category offset; 0 selects 0x50. iD uses 0x4F. */
    uint8_t  cat_off;

    /* Downloaded-game store; 0 selects kind 7. iD uses kind 5. */
    int      game_kind;
} DlcDevice;

int              dlc_device_count(void);
const DlcDevice *dlc_device_at(int i);          /* NULL past the end */
const DlcDevice *dlc_device_find(const char *name);  /* NULL if no such device */
const DlcDevice *dlc_device_default(void);      /* the P's */

/* Return 0 and report invalid device metadata on f. */
int dlc_device_check(const DlcDevice *d, FILE *f);

/* Identify a flash image by reset vector; NULL if unreadable or unknown. */
const DlcDevice *dlc_device_of_image(const char *path);

int            dlc_kind_count(const DlcDevice *d);
const DlcKind *dlc_kind_at(const DlcDevice *d, int index);
const DlcKind *dlc_kind(const DlcDevice *d, int kind);

/* Device-specific values with defaults applied; all are NULL-safe. */
uint32_t dlc_image_size(const DlcDevice *d);   /* d->image_size ? : DLC_IMAGE_SIZE */
uint8_t  dlc_cat_off(const DlcDevice *d);      /* d->cat_off    ? : 0x50 */
int      dlc_game_kind(const DlcDevice *d);    /* d->game_kind  ? : 7 */

/* The iD distinguishes games from outings with this record byte. */
#define DLC_ID_GAME_DISC_OFF  0x64
#define DLC_ID_GAME_DISC_VAL  0x37

/* Extract and verify a TAMAGO record from .jpg or .bin. Caller frees *out.
 * Returns 0, or -1 with err set. */
int dlc_extract_payload(const char *path, uint8_t **out, size_t *outlen,
                        char *err, size_t errsz);

/* Header fields: type byte [0x4E], ASCII id [0x34..0x48], UTF-16BE name
 * [0x06..0x34] converted to UTF-8 and stripped. */
void dlc_parse_header(const uint8_t *p, size_t plen, int *typ,
                      char *id, size_t idsz, char *name, size_t namesz);

/* Return a 4U item's community English name, or NULL if none is known. */
const char *dlc_4u_name_en(const char *ascii_id);

/* Map a record to a storage kind. See DlcRouteFn for output conventions. */
void dlc_route(const DlcDevice *d, int typ, int cat, const char *ascii_id,
               int flag, const uint8_t *rec, size_t reclen,
               const char **label, int *kind);

/* How full each category is. Writes up to `max` entries, returns the count. */
int dlc_store_usage(const DlcDevice *d, const uint8_t *img,
                    DlcUsage *out, int max);

/* List every slot of `kind`. Writes up to `max` entries and returns the count,
 * or 0 if the device has no such kind. Read-only. */
int dlc_store_slots(const DlcDevice *d, const uint8_t *img, int kind,
                    DlcSlot *out, int max);

/* Erase a slot to 0xFF. Returns -1 for an unknown kind or invalid slot. */
int dlc_free_slot(const DlcDevice *d, uint8_t *img, int kind, int slot);

/* Write only to a valid erased slot. Call dlc_free_slot() explicitly before a
 * replacement. Returns the image offset or -1. */
long dlc_place_at(const DlcDevice *d, uint8_t *img, int kind, int slot,
                  const uint8_t *p, size_t plen);

/* One slot to erase before installation. */
typedef struct { int kind, slot; } DlcFree;

/* Route and install payloads. Backup precedes wipes and frees. Payload errors
 * stay in res and do not abort the batch; res must hold npaths entries. Returns
 * -1 for setup or image failures, with err set. */
int dlc_inject(const DlcDevice *d, const char *target,
               const char *const *paths, int npaths,
               int wipe, int backup,
               const DlcFree *frees, int nfrees,
               DlcResult *res, char *err, size_t errsz);


/* ---- library scanning (dlc_scan.c) -------------------------------------- */

#define DLC_TAB_MAX    32
#define VDP_PARTS_MAX  4         /* a multi-part VDP+ set is 2-4 payloads */
#define DLC_PATH_MAX   520

typedef struct {
    char display[256];                        /* what the list shows */
    char tab[DLC_TAB_MAX];                    /* which tab it belongs on */
    int  nparts;
    char parts[VDP_PARTS_MAX][DLC_PATH_MAX];  /* payload path(s) */
} DlcItem;

int         dlc_tab_count(const DlcDevice *d);
const char *dlc_tab_at(const DlcDevice *d, int i);

/* Choose a tab and display name. Unreadable or unroutable payloads return an
 * empty tab and are omitted. */
void dlc_tab_for(const DlcDevice *d, const char *path, char *tab, size_t tabsz,
                 char *display, size_t dispsz);

/* Scan libdir recursively. Allocates *out (caller frees) and sets *count.
 * Returns -1 if the folder cannot be read. Sorts by tab, then display name
 * case-insensitively. */
int dlc_scan_library(const DlcDevice *d, const char *libdir,
                     DlcItem **out, int *count);

#endif
