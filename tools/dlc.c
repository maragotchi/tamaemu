#include "dlc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tamagotchi P's ------------------------------------------------------ */
static const DlcKind KINDS[] = {
    { 0,  0x5A0000, 0x0800, 20, "Meals"            },  /* Restaurant     */
    { 1,  0x5B0000, 0x0800, 15, "Snacks"           },  /* Cafe           */
    { 2,  0x610000, 0x4000,  5, "Clothing"         },  /* Tamamori Shop  */
    { 3,  0x660000, 0x2000, 10, "Accessories"      },  /* Tamamori Shop  */
    { 4,  0x6B0000, 0x2000, 10, "Items"            },  /* Tama Depa      */
    { 6,  0x6F0000, 0x2000, 10, "Wallpapers"       },  /* Gotchi Reform  */
    { 7,  0x710000, 0x8000,  2, "Games"            },  /* Game Center    */
    { 8,  0x720000, 0x8000,  2, "VDP destinations" },  /* door menu      */
    { 10, 0x750000, 0x8000,  4, "VDP recipes"      },  /* installer input*/
};
#define NKINDS ((int)(sizeof KINDS / sizeof KINDS[0]))

/* Built-in content shares these stores; wipe only games and VDP content. */
static const int WIPE_KINDS[] = { 7, 8, 10 };

static const char *const PS_TABS[] = {
    "Games", "VDP Pierces", "Meals", "Snacks", "Clothing",
    "Accessories", "Items", "Wallpapers", "Destinations",
};

/* ---- Tamagotchi iD L ----------------------------------------------------- */
static const DlcKind IDL_KINDS[] = {
    { 0, 0x590000, 0x0800, 20, "Meals"       },  /* food icon > gohan    */
    { 1, 0x5A0000, 0x0800, 20, "Snacks"      },  /* food icon > oyatsu   */
    { 2, 0x600000, 0x4000,  5, "Clothing"    },  /* Tamamori > youfuku   */
    { 3, 0x650000, 0x2000, 10, "Accessories" },  /* Tamamori > accessory */
    { 4, 0x6A0000, 0x2000, 10, "Items"       },  /* TamaDepa > asobidougu*/
    { 5, 0x740000, 0x8000,  5, "Daily Items" },  /* TamaDepa > nichiyouhin*/
    { 6, 0x780000, 0x2000, 10, "Wallpapers"  },  /* Gotchi Interior      */
    { 7, 0x7A0000, 0x8000,  2, "Games"       },  /* Game Center          */
    { 8, 0x7B0000, 0x8000,  2, "Destinations"},  /* travel, door menu    */
    /* Mailbox stores are disabled:
     *
     *   {  9, 0x7C0000, 0x0800, 20, "Mailbox"    }   letters
     *   { 10, 0x7F0000, 0x2800,  5, "Happy Mail" }   never seen on screen
     *
     * Re-enable them together with their IDL_ROUTE, IDL_TABS, and dlc_scan.c
     * LABEL_TAB entries. */
};

/* iD L built-ins live outside these stores, so every listed store is wipeable. */
static const int IDL_WIPE[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };

/* Only routable categories get tabs. */
static const char *const IDL_TABS[] = {
    "Games", "Meals", "Snacks", "Clothing", "Accessories", "Items",
    "Daily Items", "Seeds", "Wallpapers", "Destinations",
};

/* ---- Tamagotchi 4U / 4U+ -------------------------------------------------
 * Use firmware geometry because some stores overlap. Types 5 and 10 are
 * intentionally unrouted; 11-14 are not stores. Labels must fit the launcher's
 * 17-character usage column. */
static const DlcKind FOURU_KINDS[] = {
    { 15, 0x5A0000, 0x2000,  4, "Decorations"          },
    {  0, 0x5B0000, 0x0800, 20, "Meals"                },
    {  1, 0x5C0000, 0x0800, 20, "Snacks"               },
    {  2, 0x610000, 0x4000,  5, "Clothes"              },
    {  3, 0x660000, 0x2000, 10, "Accessories"          },
    /* Firmware declares 15 slots, extending kind 4 into unrouted type 5. */
    {  4, 0x6C0000, 0x2000, 15, "TamaDepa"             },
    {  6, 0x6F0000, 0x2000, 10, "Gotchi Interior"      },
    {  7, 0x710000, 0x8000,  2, "Game Center"          },
    {  8, 0x720000, 0x8000,  4, "Door Icon"            },
    { 16, 0x740000, 0x2000,  1, "Coupon"               },
    { 17, 0x750000, 0x8000,  1, "Bingo"                },
    { 18, 0x760000, 0x8000,  1, "Gashapon"             },
    /* The firmware reads characters from this bank, not the NFC install mirror
     * at 0x060000. Kind 19 is an internal id, not firmware metadata. */
    { 19, 0x780000, 0x8000,  8, "Characters"           },
};

/* Wiping kind 19 leaves the NFC mirror stale, matching a factory reset. */
static const int FOURU_WIPE[] = { 0, 1, 2, 3, 4, 6, 7, 8, 15, 16, 17, 18, 19 };

/* TamaMori combines clothing and accessories in one tab. */
static const char *const FOURU_TABS[] = {
    "Game Center", "Meals", "Snacks", "TamaMori", "Gotchi Interior", "Door Icon",
    "TamaDepa", "Decorations", "Coupon", "Bingo", "Gashapon", "Characters",
};

/* ---- Tamagotchi iD -------------------------------------------------------
 * Travel is at 0x0E0000, outside the main store range. */
static const DlcKind ID_KINDS[] = {
    { 0, 0x3C0000, 0x0800,  6, "Meals"        },  /* Restaurant            */
    { 1, 0x3C3800, 0x0800,  3, "Snacks"       },  /* TamaCafe              */
    { 2, 0x370000, 0x1800,  4, "Toys"         },  /* Tama Depa             */
    { 3, 0x3A0000, 0x0800, 10, "Letters"      },  /* Mailbox               */
    { 4, 0x390000, 0x2000,  4, "Interior"     },  /* Gotchi Reform         */
    { 5, 0x360000, 0x8000,  2, "Games"        },  /* Game Centre           */
    { 6, 0x380000, 0x2000,  5, "Outfits"      },  /* Photo Studio          */
    { 7, 0x38E000, 0x2000,  1, "Backdrop"     },  /* Photo Studio          */
    { 8, 0x3B0000, 0x2000,  3, "Accessories"  },  /* accessory shop        */
    { 9, 0x0E0000, 0x8000,  2, "Destinations" },  /* Travel / the door     */
};

/* Built-in stock lives elsewhere, so all ten stores are wipeable. */
static const int ID_WIPE[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

static const char *const ID_TABS[] = {
    "Games", "Destinations", "Meals", "Snacks", "Toys", "Letters",
    "Interior", "Outfits", "Backdrop", "Accessories",
};

/* iD routes use (group [0x4E], category [0x4F], subtype [0x51]). Doughnuts and
 * the unidentified store at 0x376000 remain unrouted rather than guessed. */
static const struct { int typ, cat, flag, kind; const char *label; } ID_ROUTE[] = {
    { 0x01, 0x01, 0x01,  0, "meal"             },  /* Restaurant           */
    { 0x01, 0x01, 0x03,  1, "snack"            },  /* TamaCafe             */
    { 0x01, 0x04, 0x01,  2, "tama depa toy"    },
    { 0x01, 0x06, 0x00,  3, "letter"           },  /* Mailbox              */
    { 0x01, 0x07, 0x00,  4, "reform wallpaper" },  /* Gotchi Reform        */
    { 0x01, 0x02, 0x00,  8, "accessory"        },
    { 0x02, 0x03, 0x01,  6, "outfit"           },  /* Photo Studio         */
    { 0x02, 0x03, 0x02,  7, "backdrop"         },  /* Photo Studio         */
    { 0x01, 0x01, 0x02, -1, "doughnut (no store found on the device yet)" },
};

static void dlc_route_id(int typ, int cat, const char *ascii_id, int flag,
                         const uint8_t *rec, size_t reclen,
                         const char **label, int *kind)
{
    (void)ascii_id;    /* empty on every iD download, so it can split nothing */
    /* iD games and outings share a header. Byte 0x64 selects Game Centre for
     * 0x37 and Travel otherwise; short records remain unroutable. */
    if (typ == 0x14 && cat == 0x02 && flag == 0x00) {
        if (!rec || reclen <= DLC_ID_GAME_DISC_OFF) {
            *label = "game or outing, but the record is too short to say which";
            *kind = -1;
        } else if (rec[DLC_ID_GAME_DISC_OFF] == DLC_ID_GAME_DISC_VAL) {
            *label = "game";   *kind = 5;
        } else {
            *label = "outing"; *kind = 9;
        }
        return;
    }
    for (size_t i = 0; i < sizeof ID_ROUTE / sizeof ID_ROUTE[0]; i++)
        if (ID_ROUTE[i].typ == typ && ID_ROUTE[i].cat == cat &&
            ID_ROUTE[i].flag == flag) {
            *label = ID_ROUTE[i].label; *kind = ID_ROUTE[i].kind;
            return;
        }
    *label = NULL; *kind = -1;
}

static void dlc_route_ps(int typ, int cat, const char *ascii_id, int flag,
                         const uint8_t *rec, size_t reclen,
                         const char **label, int *kind);
static void dlc_route_idl(int typ, int cat, const char *ascii_id, int flag,
                          const uint8_t *rec, size_t reclen,
                          const char **label, int *kind);
static void dlc_route_4u(int typ, int cat, const char *ascii_id, int flag,
                         const uint8_t *rec, size_t reclen,
                         const char **label, int *kind);

static const DlcDevice DEVICES[] = {
    {
        .name = "ps", .title = "Tamagotchi P's", .shortname = "P's",
        .reset_vec = 0x024FDFE6u,
        .kinds = KINDS,      .nkinds = NKINDS,
        .wipe  = WIPE_KINDS, .nwipe  = (int)(sizeof WIPE_KINDS / sizeof WIPE_KINDS[0]),
        .tabs  = PS_TABS,    .ntabs  = (int)(sizeof PS_TABS / sizeof PS_TABS[0]),
        .route = dlc_route_ps,
    },
    {
        .name = "idl", .title = "Tamagotchi iD L", .shortname = "iD L",
        .reset_vec = 0x024CA8FEu,
        .kinds = IDL_KINDS,  .nkinds = (int)(sizeof IDL_KINDS / sizeof IDL_KINDS[0]),
        .wipe  = IDL_WIPE,   .nwipe  = (int)(sizeof IDL_WIPE / sizeof IDL_WIPE[0]),
        .tabs  = IDL_TABS,   .ntabs  = (int)(sizeof IDL_TABS / sizeof IDL_TABS[0]),
        .route = dlc_route_idl,
    },
    {
        .name = "id", .title = "Tamagotchi iD", .shortname = "iD",
        .reset_vec = 0x00C907FAu,
        .kinds = ID_KINDS,   .nkinds = (int)(sizeof ID_KINDS / sizeof ID_KINDS[0]),
        .wipe  = ID_WIPE,    .nwipe  = (int)(sizeof ID_WIPE / sizeof ID_WIPE[0]),
        .tabs  = ID_TABS,    .ntabs  = (int)(sizeof ID_TABS / sizeof ID_TABS[0]),
        .route = dlc_route_id,
        .image_size = 4u * 1024u * 1024u,
        .cat_off = 0x4F,
        .game_kind = 5,
    },
    /* iD Melody shares the iD stores and routing. Keep game_kind at 5; the
     * default value would route games to Backdrop. */
    {
        .name = "id-melody", .title = "Tamagotchi iD Melody",
        .shortname = "iD Melody",
        .reset_vec = 0x00C939EAu,
        .kinds = ID_KINDS,   .nkinds = (int)(sizeof ID_KINDS / sizeof ID_KINDS[0]),
        .wipe  = ID_WIPE,    .nwipe  = (int)(sizeof ID_WIPE / sizeof ID_WIPE[0]),
        .tabs  = ID_TABS,    .ntabs  = (int)(sizeof ID_TABS / sizeof ID_TABS[0]),
        .route = dlc_route_id,
        .image_size = 4u * 1024u * 1024u,
        .cat_off = 0x4F,
        .game_kind = 5,
    },
    /* 4U and 4U+ share content geometry but use different save signatures. */
    {
        .name = "4u", .title = "Tamagotchi 4U+", .shortname = "4U+",
        .reset_vec = 0x024F6256u,
        .kinds = FOURU_KINDS, .nkinds = (int)(sizeof FOURU_KINDS / sizeof FOURU_KINDS[0]),
        .wipe  = FOURU_WIPE,  .nwipe  = (int)(sizeof FOURU_WIPE / sizeof FOURU_WIPE[0]),
        .tabs  = FOURU_TABS,  .ntabs  = (int)(sizeof FOURU_TABS / sizeof FOURU_TABS[0]),
        .route = dlc_route_4u,
    },
    {
        .name = "4u-plain", .title = "Tamagotchi 4U", .shortname = "4U",
        .reset_vec = 0x024C680Au,
        .kinds = FOURU_KINDS, .nkinds = (int)(sizeof FOURU_KINDS / sizeof FOURU_KINDS[0]),
        .wipe  = FOURU_WIPE,  .nwipe  = (int)(sizeof FOURU_WIPE / sizeof FOURU_WIPE[0]),
        .tabs  = FOURU_TABS,  .ntabs  = (int)(sizeof FOURU_TABS / sizeof FOURU_TABS[0]),
        .route = dlc_route_4u,
    },
    /* Plus Color uses codes rather than installable records. */
    {
        .name = "plus-color", .title = "Tamagotchi Plus Color",
        .shortname = "Plus Color",
        .reset_vec = 0x00C2F0ECu,
        .image_size = 4u * 1024u * 1024u,
    },
    /* Hexagontchi shares the Plus Color layout but has its own signature. */
    {
        .name = "plus-color-hexa", .title = "Tamagotchi Plus Color (Hexagontchi)",
        .shortname = "Hexagontchi",
        .reset_vec = 0x00C4215Eu,
        .image_size = 4u * 1024u * 1024u,
    },
};
#define NDEVICES ((int)(sizeof DEVICES / sizeof DEVICES[0]))

int dlc_device_count(void) { return NDEVICES; }

const DlcDevice *dlc_device_at(int i)
{
    return (i >= 0 && i < NDEVICES) ? &DEVICES[i] : NULL;
}

const DlcDevice *dlc_device_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < NDEVICES; i++)
        if (!strcmp(DEVICES[i].name, name)) return &DEVICES[i];
    return NULL;
}

const DlcDevice *dlc_device_default(void) { return &DEVICES[0]; }

const DlcDevice *dlc_device_of_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t b[4];
    /* 8 MB images (P's, iD L, 4U): vector table at file offset 0x400000 */
    int ok = (fseek(f, 0x400000L, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4);
    if (!ok) {
        /* 4 MB images (iD): vector table at file offset 0 */
        ok = (fseek(f, 0L, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4);
    }
    fclose(f);
    if (!ok) return NULL;
    uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    for (int i = 0; i < NDEVICES; i++)
        if (DEVICES[i].reset_vec && DEVICES[i].reset_vec == v) return &DEVICES[i];
    return NULL;
}

int dlc_device_check(const DlcDevice *d, FILE *f)
{
    if (!d) { fprintf(f, "[dlc] (no device)\n"); return 0; }
    int ok = 1;

    if (!d->shortname || !d->shortname[0]) {
        fprintf(f, "[dlc] %s: no shortname - its device tab would be blank\n",
                d->name);
        ok = 0;
    }

    struct { const char *what; int n; const void *p; } pairs[] = {
        { "kinds", d->nkinds, (const void *)d->kinds },
        { "tabs",  d->ntabs,  (const void *)d->tabs  },
        { "wipe",  d->nwipe,  (const void *)d->wipe  },
    };
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        if (pairs[i].n < 0) {
            fprintf(f, "[dlc] %s: n%s is %d\n", d->name, pairs[i].what, pairs[i].n);
            ok = 0;
        } else if ((pairs[i].n > 0) != (pairs[i].p != NULL)) {
            fprintf(f, "[dlc] %s: n%s is %d but %s is %s - a half-filled row\n",
                    d->name, pairs[i].what, pairs[i].n, pairs[i].what,
                    pairs[i].p ? "set" : "NULL");
            ok = 0;
        }
    }

    if ((d->nkinds > 0) != (d->route != NULL)) {
        fprintf(f, "[dlc] %s: nkinds is %d but route is %s - stores and routing "
                   "have to arrive together\n",
                d->name, d->nkinds, d->route ? "set" : "NULL");
        ok = 0;
    }

    for (int i = 0; i < d->nkinds && d->kinds; i++) {
        const DlcKind *k = &d->kinds[i];
        if (k->slotsz == 0 || k->nslots <= 0) {
            fprintf(f, "[dlc] %s: kind %d has %d slots of %u bytes\n",
                    d->name, k->kind, k->nslots, k->slotsz);
            ok = 0;
            continue;
        }
        uint64_t end = (uint64_t)k->base + (uint64_t)k->nslots * k->slotsz;
        if (end > dlc_image_size(d)) {
            fprintf(f, "[dlc] %s: kind %d ends at 0x%llx, past the %u-byte image\n",
                    d->name, k->kind, (unsigned long long)end, dlc_image_size(d));
            ok = 0;
        }
        for (int j = 0; j < i; j++)
            if (d->kinds[j].kind == k->kind) {
                fprintf(f, "[dlc] %s: kind %d appears twice\n", d->name, k->kind);
                ok = 0;
            }
        for (int j = 0; j < i; j++) {
            const DlcKind *o = &d->kinds[j];
            uint64_t oend = (uint64_t)o->base + (uint64_t)o->nslots * o->slotsz;
            if (k->base < oend && o->base < end) {
                fprintf(f, "[dlc] %s: kinds %d and %d overlap in flash\n",
                        d->name, o->kind, k->kind);
                ok = 0;
            }
        }
    }

    for (int i = 0; i < d->nwipe && d->wipe; i++)
        if (!dlc_kind(d, d->wipe[i])) {
            fprintf(f, "[dlc] %s: wipe lists kind %d, which has no store\n",
                    d->name, d->wipe[i]);
            ok = 0;
        }

    return ok;
}

int dlc_kind_count(const DlcDevice *d) { return d ? d->nkinds : 0; }

const DlcKind *dlc_kind_at(const DlcDevice *d, int index)
{
    return (d && index >= 0 && index < d->nkinds) ? &d->kinds[index] : NULL;
}

const DlcKind *dlc_kind(const DlcDevice *d, int kind)
{
    if (!d) return NULL;
    for (int i = 0; i < d->nkinds; i++)
        if (d->kinds[i].kind == kind) return &d->kinds[i];
    return NULL;
}

uint32_t dlc_image_size(const DlcDevice *d)
{
    return (d && d->image_size) ? d->image_size : DLC_IMAGE_SIZE;
}

uint8_t dlc_cat_off(const DlcDevice *d)
{
    return (d && d->cat_off) ? d->cat_off : 0x50;
}

int dlc_game_kind(const DlcDevice *d)
{
    return (d && d->game_kind) ? d->game_kind : 7;
}

/* (type [0x4E], category [0x50]) -> (label, kind). Games and VDP destinations
 * share type 0x94 / category 0x5b and are split in dlc_route(). */
static const struct { int typ, cat, kind; const char *label; } CATEGORY_KIND[] = {
    { 0x81, 0x02,  0, "meal"                   },
    { 0x81, 0x0D,  1, "snack"                  },
    { 0x81, 0x33,  2, "clothing"               },
    { 0x81, 0x34,  3, "accessory"              },
    { 0x81, 0x35,  3, "accessory"              },
    { 0x81, 0x2A,  4, "item"                   },
    { 0x81, 0x1F,  6, "wallpaper"              },
    { 0x81, 0x29, 10, "VDP recipe extension"   },
    /* Category 0x47 distinguishes travel from VDP change destinations that
     * otherwise share type 0x94 and flag 0x02. */
    { 0x94, 0x47,  8, "travel destination"     },
};
/* Recognized categories without mapped stores. */
static const struct { int typ, cat; const char *label; } PENDING[] = {
    { 0x81, 0x52, "stamp card (store not mapped yet)"         },
};

static void dlc_route_ps(int typ, int cat, const char *ascii_id, int flag,
                         const uint8_t *rec, size_t reclen,
                         const char **label, int *kind)
{
    (void)rec; (void)reclen;
    /* Flag 0x01 selects games; other flags select VDP destinations. */
    if (typ == 0x94 && cat == 0x5B) {
        if (flag == 0x01) { *label = "game";            *kind = 7; }
        else              { *label = "VDP destination"; *kind = 8; }
        return;
    }
    /* Category 0x29 is shared: VDP recipe extensions (id 'DecoPierceLoader')
     * vs standalone Connection-Play recipes (id 'idn_rec*'), a distinct
     * category whose store is not mapped. Split by id so recipes do not land
     * in the VDP extension slots. */
    if (typ == 0x81 && cat == 0x29 && strncmp(ascii_id, "idn_rec", 7) == 0) {
        *label = "recipe (store not mapped yet)"; *kind = -1;
        return;
    }
    for (size_t i = 0; i < sizeof CATEGORY_KIND / sizeof CATEGORY_KIND[0]; i++)
        if (CATEGORY_KIND[i].typ == typ && CATEGORY_KIND[i].cat == cat) {
            *label = CATEGORY_KIND[i].label; *kind = CATEGORY_KIND[i].kind;
            return;
        }
    for (size_t i = 0; i < sizeof PENDING / sizeof PENDING[0]; i++)
        if (PENDING[i].typ == typ && PENDING[i].cat == cat) {
            *label = PENDING[i].label; *kind = -1;
            return;
        }
    *label = NULL; *kind = -1;
}

/* iD L routes use the full (type [0x4E], category [0x50], flag [0x51]) key;
 * category 0x29 uses its flag to distinguish seeds from daily items. Kind -1
 * means the payload is recognized but its device store is still unknown. */
static const struct { int typ, cat, flag, kind; const char *label; } IDL_ROUTE[] = {
    { 0x81, 0x02, 0x01,  0, "meal"                                              },
    { 0x81, 0x0C, 0x02,  1, "snack"                                             },
    { 0x81, 0x0D, 0x02,  1, "snack"                                             },
    { 0x81, 0x1F, 0x00,  6, "wallpaper"                                         },
    { 0x81, 0x29, 0x00,  5, "seed"                                              },
    { 0x81, 0x29, 0x02,  5, "daily item"                                        },
    { 0x81, 0x2A, 0x01,  4, "item"                                              },
    { 0x81, 0x33, 0x00,  2, "clothing"                                          },
    /* Accessories use either category 0x34 or 0x35. */
    { 0x81, 0x34, 0x00,  3, "accessory"                                         },
    { 0x81, 0x35, 0x00,  3, "accessory"                                         },
    /* Mail is recognized but disabled with its stores above. */
    { 0x81, 0x51, 0x01, -1, "letter (mail is off for now)"                      },
    { 0x81, 0x52, 0x02, -1, "happy mail (mail is off for now)"                  },
    { 0x94, 0x47, 0x02,  8, "travel destination"                                },
    { 0x94, 0x5B, 0x01,  7, "game"                                              },
};

static void dlc_route_idl(int typ, int cat, const char *ascii_id, int flag,
                         const uint8_t *rec, size_t reclen,
                         const char **label, int *kind)
{
    (void)rec; (void)reclen;
    (void)ascii_id;
    for (size_t i = 0; i < sizeof IDL_ROUTE / sizeof IDL_ROUTE[0]; i++)
        if (IDL_ROUTE[i].typ == typ && IDL_ROUTE[i].cat == cat &&
            IDL_ROUTE[i].flag == flag) {
            *label = IDL_ROUTE[i].label; *kind = IDL_ROUTE[i].kind;
            return;
        }
    *label = NULL; *kind = -1;
}

/* 4U routes use (type [0x4E], category [0x50], flag [0x51]). Numeric IDs
 * resolve colliding headers in dlc_route_4u(). Kind -1 marks recognized content
 * without a mapped store. */
static const struct { int typ, cat, flag, kind; const char *label; } FOURU_ROUTE[] = {
    { 0x81, 0x01, 0x01,  0, "meal"                                             },
    { 0x81, 0x02, 0x01,  0, "meal"                                             },
    { 0x81, 0x01, 0x02,  1, "snack"                                            },
    { 0x81, 0x0D, 0x02,  1, "snack"                                            },
    { 0x81, 0x1F, 0x00,  6, "interior"                                         },
    { 0x81, 0x33, 0x00,  2, "tamamori outfit"                                  },
    { 0x81, 0x34, 0x00,  3, "tamamori accessory"                               },
    { 0x81, 0x35, 0x00,  3, "tamamori accessory"                               },
    /* The flag distinguishes minigames from outings that share a header. */
    { 0x94, 0x5B, 0x01,  7, "minigame"                                         },
    { 0x94, 0x5B, 0x02,  8, "outing destination"                               },
    { 0x94, 0x48, 0x03, 19, "character"                                        },
    /* App outings and EP1 outings use different headers but share kind 8. */
    { 0x94, 0x47, 0x02,  8, "outing destination"                               },
};

/* Read the first run of at least two digits from an ASCII id. A single digit
 * would incorrectly select the 4 in the t4u prefix. */
static int fouru_item_id(const char *s)
{
    if (!s) return -1;
    for (; *s; s++)
        if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') {
            long n = 0;
            for (; *s >= '0' && *s <= '9'; s++) {
                n = n * 10 + (*s - '0');
                if (n > 0xFFFF) return -1;
            }
            return (int)n;
        }
    return -1;
}

static void dlc_route_4u(int typ, int cat, const char *ascii_id, int flag,
                         const uint8_t *rec, size_t reclen,
                         const char **label, int *kind)
{
    (void)rec; (void)reclen;
    /* Colliding 4U headers route by the numeric id windows used by firmware. */
    if (typ == 0x81 && cat == 0x2A && flag == 0x01) {
        int id = fouru_item_id(ascii_id);
        int m = id % 1000;
        if (id < 0) {
            *label = "toy, coupon or decoration with no number in its id - "
                     "the id is what picks the store";
            *kind = -1;
        } else if (m >= 601 && m <= 749) {
            *label = "coupon";     *kind = 16;
        } else if (m >= 801 && m <= 949) {
            *label = "decoration"; *kind = 15;
        } else {
            *label = "toy";        *kind = 4;   /* the firmware's default */
        }
        return;
    }
    if (typ == 0x81 && cat == 0x29 && flag == 0x00) {
        int id = fouru_item_id(ascii_id);
        int m = id % 1000;
        if (id >= 0 && m >= 301 && m <= 449) {
            *label = "bingo";    *kind = 17;
        } else if (id >= 0 && m >= 601 && m <= 749) {
            *label = "gashapon"; *kind = 18;
        } else {
            *label = "bingo or gashapon outside both measured id windows";
            *kind = -1;
        }
        return;
    }
    for (size_t i = 0; i < sizeof FOURU_ROUTE / sizeof FOURU_ROUTE[0]; i++)
        if (FOURU_ROUTE[i].typ == typ && FOURU_ROUTE[i].cat == cat &&
            FOURU_ROUTE[i].flag == flag) {
            *label = FOURU_ROUTE[i].label; *kind = FOURU_ROUTE[i].kind;
            return;
        }
    *label = NULL; *kind = -1;
}

void dlc_route(const DlcDevice *d, int typ, int cat, const char *ascii_id,
               int flag, const uint8_t *rec, size_t reclen,
               const char **label, int *kind)
{
    *label = NULL; *kind = -1;
    if (!d || !d->route) return;
    d->route(typ, cat, ascii_id, flag, rec, reclen, label, kind);
}

/* ---- header parsing ------------------------------------------------------ */

/* Include Unicode whitespace such as U+3000 used in Japanese names. */
static int py_isspace(uint32_t c)
{
    return c == 0x20 || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

static size_t utf8_put(char *d, size_t cap, size_t n, uint32_t c)
{
    if (c < 0x80) {
        if (n + 1 >= cap) return n;
        d[n++] = (char)c;
    } else if (c < 0x800) {
        if (n + 2 >= cap) return n;
        d[n++] = (char)(0xC0 | (c >> 6));
        d[n++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        if (n + 3 >= cap) return n;
        d[n++] = (char)(0xE0 | (c >> 12));
        d[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
        d[n++] = (char)(0x80 | (c & 0x3F));
    } else {
        if (n + 4 >= cap) return n;
        d[n++] = (char)(0xF0 | (c >> 18));
        d[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
        d[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
        d[n++] = (char)(0x80 | (c & 0x3F));
    }
    return n;
}

void dlc_parse_header(const uint8_t *p, size_t plen, int *typ,
                      char *id, size_t idsz, char *name, size_t namesz)
{
    if (typ) *typ = (plen > 0x4E) ? p[0x4E] : -1;

    /* ASCII id: 0x34..0x48, NUL-terminated, latin1. */
    if (id && idsz) {
        size_t n = 0;
        for (size_t i = 0x34; i < 0x48 && i < plen && n + 1 < idsz; i++) {
            if (p[i] == 0) break;
            id[n++] = (char)p[i];
        }
        id[n] = '\0';
    }

    /* Name: 0x06..0x34 as UTF-16BE -> UTF-8, trailing NULs removed, then
     * stripped of whitespace at both ends (Python: .rstrip("\0").strip()). */
    if (name && namesz) {
        uint32_t cps[32];
        int ncp = 0;
        for (size_t i = 6; i + 1 < 0x34 && i + 1 < plen && ncp < 32; i += 2) {
            uint32_t u = ((uint32_t)p[i] << 8) | p[i + 1];
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < 0x34 && i + 3 < plen) {
                uint32_t lo = ((uint32_t)p[i + 2] << 8) | p[i + 3];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cps[ncp++] = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                    continue;
                }
                u = 0xFFFD;                    /* unpaired high surrogate */
            } else if (u >= 0xD800 && u <= 0xDFFF) {
                u = 0xFFFD;                    /* unpaired surrogate */
            }
            cps[ncp++] = u;
        }
        while (ncp > 0 && cps[ncp - 1] == 0) ncp--;            /* trailing NUL */
        int a = 0;
        while (a < ncp && py_isspace(cps[a])) a++;             /* leading space */
        while (ncp > a && py_isspace(cps[ncp - 1])) ncp--;     /* trailing space */
        size_t n = 0;
        for (int i = a; i < ncp; i++) n = utf8_put(name, namesz, n, cps[i]);
        name[n] = '\0';
    }
}

/* ---- payload extraction -------------------------------------------------- */

static const uint8_t *memfind(const uint8_t *h, size_t hn,
                              const uint8_t *n, size_t nn)
{
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(h + i, n, nn) == 0) return h + i;
    return NULL;
}

/* Records end with a big-endian 16-bit sum of the preceding bytes. */
static int mac_ok(const uint8_t *b, size_t len)
{
    if (len < 3) return 0;
    uint32_t mac = ((uint32_t)b[len - 2] << 8) | b[len - 1];
    uint32_t sum = 0;
    for (size_t i = 0; i < len - 2; i++) sum += b[i];
    return (sum & 0xFFFFu) == mac;
}

int dlc_extract_payload(const char *path, uint8_t **out, size_t *outlen,
                        char *err, size_t errsz)
{
    *out = NULL; *outlen = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open file"); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); snprintf(err, errsz, "empty file"); return -1; }
    if ((unsigned long long)sz > DLC_PAYLOAD_MAX) {
        fclose(f);
        snprintf(err, errsz, "payload too large (maximum %u bytes)",
                 (unsigned)DLC_PAYLOAD_MAX);
        return -1;
    }
    uint8_t *d = malloc((size_t)sz);
    if (!d) { fclose(f); snprintf(err, errsz, "out of memory"); return -1; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(d); snprintf(err, errsz, "short read"); return -1;
    }
    fclose(f);

    const uint8_t *blob;
    size_t blobn;
    if ((size_t)sz >= 6 && memcmp(d, "TAMAGO", 6) == 0) {
        blob = d; blobn = (size_t)sz;
    } else {
        /* The TAMAGO payload is appended after the JPEG, usually right at the
         * end-of-image marker but sometimes a few padding bytes later. Find the
         * first EOI, then the next magic at or after it. */
        static const uint8_t EOI[2] = { 0xFF, 0xD9 };
        const uint8_t *e = memfind(d, (size_t)sz, EOI, 2);
        const uint8_t *m;
        if (e) {
            size_t after = (size_t)(e - d) + 2;
            m = memfind(d + after, (size_t)sz - after, (const uint8_t *)"TAMAGO", 6);
        } else {
            m = memfind(d, (size_t)sz, (const uint8_t *)"TAMAGO", 6);
        }
        if (!m) {
            free(d);
            snprintf(err, errsz, "no TAMAGO payload found after JPEG EOI");
            return -1;
        }
        blob = m; blobn = (size_t)sz - (size_t)(m - d);
    }

    /* Prefer the big-endian length at +0x48. Some valid 4U records understate
     * it, so a failed checksum may fall back to the full blob, but never to a
     * shorter record and never without a valid checksum. */
    size_t declared = 0;
    if (blobn >= 0x4C) {
        size_t ln = ((size_t)blob[0x48] << 24) | ((size_t)blob[0x49] << 16) |
                    ((size_t)blob[0x4A] <<  8) |  (size_t)blob[0x4B];
        if (ln >= 0x60 && ln <= blobn) declared = ln;
    }

    size_t len = 0;
    if (declared && mac_ok(blob, declared)) {
        len = declared;
    } else {
        for (size_t slack = 0; slack <= 16 && slack < blobn; slack++) {
            size_t cand = blobn - slack;
            if (cand < 0x60 || cand < declared) continue;
            if (mac_ok(blob, cand)) { len = cand; break; }
        }
    }
    if (!len) {
        free(d);
        snprintf(err, errsz, "TAMAGO MAC mismatch - corrupt payload");
        return -1;
    }

    uint8_t *p = malloc(len);
    if (!p) { free(d); snprintf(err, errsz, "out of memory"); return -1; }
    memcpy(p, blob, len);
    free(d);
    *out = p; *outlen = len;
    return 0;
}

/* ---- store inspection and placement -------------------------------------- */

int dlc_store_usage(const DlcDevice *d, const uint8_t *img,
                    DlcUsage *out, int max)
{
    if (!d || d->nkinds == 0) return 0;
    int n = 0;
    for (int i = 0; i < d->nkinds && n < max; i++) {
        const DlcKind *k = &d->kinds[i];
        int used = 0;
        for (int s = 0; s < k->nslots; s++) {
            size_t off = k->base + (size_t)s * k->slotsz;
            if (off + 6 <= dlc_image_size(d) && memcmp(img + off, "TAMAGO", 6) == 0)
                used++;
        }
        out[n].label = k->label; out[n].used = used; out[n].max = k->nslots;
        n++;
    }
    return n;
}

int dlc_store_slots(const DlcDevice *d, const uint8_t *img, int kind,
                    DlcSlot *out, int max)
{
    const DlcKind *k = dlc_kind(d, kind);
    if (!k) return 0;
    int n = 0;
    for (int s = 0; s < k->nslots && n < max; s++) {
        size_t off = k->base + (size_t)s * k->slotsz;
        if (off + k->slotsz > dlc_image_size(d)) break;
        DlcSlot *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->slot = s;
        o->off  = (long)off;
        o->occupied = (memcmp(img + off, "TAMAGO", 6) == 0);
        if (o->occupied) {
            dlc_parse_header(img + off, k->slotsz, NULL,
                             o->id, sizeof o->id, o->name, sizeof o->name);
        }
    }
    return n;
}

int dlc_free_slot(const DlcDevice *d, uint8_t *img, int kind, int slot)
{
    const DlcKind *k = dlc_kind(d, kind);
    if (!k || slot < 0 || slot >= k->nslots) return -1;
    size_t off = k->base + (size_t)slot * k->slotsz;
    if (off + k->slotsz > dlc_image_size(d)) return -1;
    memset(img + off, 0xFF, k->slotsz);
    return 0;
}

long dlc_place_at(const DlcDevice *d, uint8_t *img, int kind, int slot,
                  const uint8_t *p, size_t plen)
{
    const DlcKind *k = dlc_kind(d, kind);
    if (!k || slot < 0 || slot >= k->nslots) return -1;
    if (plen > k->slotsz) return -1;
    size_t off = k->base + (size_t)slot * k->slotsz;
    if (off + plen > dlc_image_size(d)) return -1;
    if (memcmp(img + off, "TAMAGO", 6) == 0) return -1;
    for (size_t i = 0; i < plen; i++)
        if (img[off + i] != 0xFF) return -1;
    memcpy(img + off, p, plen);
    return (long)off;
}

/* Byte-identical record already present? Compares the full record, so two
 * different items that share an id (e.g. two VDPs both 'DecoPierceLoader') are
 * not treated as duplicates. */
static int already_installed(const uint8_t *img, size_t imgsz, const DlcKind *k,
                             const uint8_t *p, size_t plen)
{
    for (int s = 0; s < k->nslots; s++) {
        size_t off = k->base + (size_t)s * k->slotsz;
        if (off + plen > imgsz) continue;
        if (memcmp(img + off, "TAMAGO", 6) == 0 && memcmp(img + off, p, plen) == 0)
            return 1;
    }
    return 0;
}

/* Use the first fully erased slot; never overwrite existing data. */
static long place(uint8_t *img, size_t imgsz, const DlcKind *k,
                  const uint8_t *p, size_t plen)
{
    if (plen > k->slotsz) return -1;
    for (int s = 0; s < k->nslots; s++) {
        size_t off = k->base + (size_t)s * k->slotsz;
        if (off + plen > imgsz) continue;
        if (memcmp(img + off, "TAMAGO", 6) == 0) continue;
        int erased = 1;
        for (size_t i = 0; i < plen; i++)
            if (img[off + i] != 0xFF) { erased = 0; break; }
        if (!erased) continue;
        memcpy(img + off, p, plen);
        return (long)off;
    }
    return -1;
}

/* ---- inject -------------------------------------------------------------- */

static const char *basename_of(const char *path)
{
    const char *b = path;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb");
    if (!a) return -1;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    fclose(a);
    if (fclose(b) != 0) ok = 0;
    return ok ? 0 : -1;
}

int dlc_inject(const DlcDevice *d, const char *target,
               const char *const *paths, int npaths,
               int wipe, int backup,
               const DlcFree *frees, int nfrees,
               DlcResult *res, char *err, size_t errsz)
{
    /* Enforce install support even if the launcher check is bypassed. */
    if (!d || d->nkinds == 0) {
        snprintf(err, errsz, "%s has no known download stores - nothing can be "
                             "installed onto it yet", d ? d->title : "(no device)");
        return -1;
    }

    /* Reject saves for another device; unknown signatures remain allowed. */
    const DlcDevice *img_dev = dlc_device_of_image(target);
    if (img_dev && img_dev != d) {
        snprintf(err, errsz, "%s looks like a %s save, but the selected device "
                             "is %s - nothing was written",
                 target, img_dev->title, d->title);
        return -1;
    }

    const uint32_t imgsz = dlc_image_size(d);
    FILE *f = fopen(target, "rb");
    if (!f) { snprintf(err, errsz, "%s: cannot open", target); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != (long)imgsz) {
        fclose(f);
        snprintf(err, errsz, "%s: expected %uMB flash image, got %ld bytes",
                 target, imgsz >> 20, sz);
        return -1;
    }
    uint8_t *img = malloc(imgsz);
    if (!img) { fclose(f); snprintf(err, errsz, "out of memory"); return -1; }
    if (fread(img, 1, imgsz, f) != imgsz) {
        fclose(f); free(img);
        snprintf(err, errsz, "%s: short read", target);
        return -1;
    }
    fclose(f);

    if (backup) {
        char bak[512];
        snprintf(bak, sizeof bak, "%s.bak", target);
        copy_file(target, bak);           /* backup failure is non-fatal */
    }

    if (wipe) {
        for (int i = 0; d && i < d->nwipe; i++) {
            const DlcKind *k = dlc_kind(d, d->wipe[i]);
            if (!k) continue;
            size_t n = (size_t)k->nslots * k->slotsz;
            if (k->base + n <= imgsz) memset(img + k->base, 0xFF, n);
        }
    }

    /* Free selected slots after backup and before installing replacements. */
    for (int i = 0; i < nfrees && frees; i++)
        if (frees[i].slot >= 0)
            dlc_free_slot(d, img, frees[i].kind, frees[i].slot);

    for (int i = 0; i < npaths; i++) {
        DlcResult *r = &res[i];
        memset(r, 0, sizeof *r);
        snprintf(r->file, sizeof r->file, "%s", basename_of(paths[i]));
        r->type = -1; r->kind = -1; r->off = -1; r->label = NULL;

        uint8_t *p = NULL;
        size_t plen = 0;
        char perr[DLC_ERR_MAX];
        /* A bad payload does not abort the batch. */
        if (dlc_extract_payload(paths[i], &p, &plen, perr, sizeof perr) != 0) {
            snprintf(r->error, sizeof r->error, "%s", perr);
            continue;
        }
        dlc_parse_header(p, plen, &r->type, r->id, sizeof r->id,
                         r->name, sizeof r->name);
        int cat  = (plen > dlc_cat_off(d)) ? p[dlc_cat_off(d)] : -1;
        int flag = (plen > 0x51) ? p[0x51] : 0;
        dlc_route(d, r->type, cat, r->id, flag, p, plen, &r->label, &r->kind);

        if (r->kind < 0) {
            snprintf(r->error, sizeof r->error, "%s",
                     r->label ? r->label : "no route for this header");
        } else {
            const DlcKind *k = dlc_kind(d, r->kind);
            if (!k) {
                snprintf(r->error, sizeof r->error, "no route for this header");
            } else if (already_installed(img, imgsz, k, p, plen)) {
                snprintf(r->error, sizeof r->error, "already installed (skipped)");
            } else {
                long off = place(img, imgsz, k, p, plen);
                if (off < 0)
                    snprintf(r->error, sizeof r->error,
                             "kind %d store full or item too big", r->kind);
                else
                    r->off = off;
            }
        }
        free(p);
    }

    FILE *w = fopen(target, "wb");
    if (!w) { free(img); snprintf(err, errsz, "%s: cannot write", target); return -1; }
    size_t wrote = fwrite(img, 1, imgsz, w);
    int ok = (wrote == imgsz) && (fclose(w) == 0);
    free(img);
    if (!ok) { snprintf(err, errsz, "%s: write failed", target); return -1; }
    return 0;
}
