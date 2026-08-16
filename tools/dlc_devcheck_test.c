
/* Positive control for dlc_device_check(): every invariant, deliberately
 * broken, one row each - plus a correct row as the negative control.
 *
 * A checker that only ever runs against a table that already passes returns 1
 * forever and looks like a guarantee. This is what says it can fail. It builds
 * its own DlcDevice rows rather than reading DEVICES[], so it keeps working
 * when a real device row changes.
 *
 * Built and run by the device-table validation command. Exit 0 = all broken rows caught AND the
 * good row passes; a MISS line names any invariant that stopped firing. */

#include "dlc.h"
#include <string.h>
static const DlcKind K1[] = { { 7, 0x7A0000, 0x8000, 2, "Games" } };
static const DlcKind KDUP[] = { { 7, 0x100000, 0x1000, 2, "a" },
                                { 7, 0x200000, 0x1000, 2, "b" } };
static const DlcKind KOVL[] = { { 7, 0x100000, 0x1000, 4, "a" },
                                { 8, 0x102000, 0x1000, 4, "b" } };
static const DlcKind KEND[] = { { 7, 0x7FF000, 0x8000, 4, "past the end" } };
/* Fits an 8 MB image, not a 4 MB one - caught only if the checker asks the
 * device for its size instead of assuming DLC_IMAGE_SIZE. */
static const DlcKind KEND4[] = { { 7, 0x3FF000, 0x8000, 2, "past 4 MB" } };
static const DlcKind KZERO[] = { { 7, 0x100000, 0, 0, "empty" } };
static const char *const T[] = { "Games" };
static const int W1[] = { 7 };
static const int W9[] = { 9 };
static void r(int t, int c, const char *i, int f, const uint8_t *b, size_t n, const char **l, int *k)
{(void)t;(void)c;(void)i;(void)f;(void)b;(void)n;*l = NULL;*k = -1;}
#define ROW(nm, ...) { .name = nm, .title = nm, .shortname = nm, \
                       .reset_vec = 0, __VA_ARGS__ }
int main(void)
{
    const DlcDevice bad[] = {
        ROW("kinds-null",  .nkinds=1, .kinds=NULL, .tabs=T, .ntabs=1, .route=r),
        ROW("kinds-orphan",.nkinds=0, .kinds=K1,   .tabs=T, .ntabs=1),
        ROW("tabs-null",   .nkinds=1, .kinds=K1,   .tabs=NULL, .ntabs=2, .route=r),
        ROW("wipe-null",   .nkinds=1, .kinds=K1,   .tabs=T, .ntabs=1, .route=r, .nwipe=1, .wipe=NULL),
        ROW("no-route",    .nkinds=1, .kinds=K1,   .tabs=T, .ntabs=1),
        ROW("route-nokinds",.nkinds=0, .tabs=T, .ntabs=1, .route=r),
        ROW("dup-kind",    .nkinds=2, .kinds=KDUP, .tabs=T, .ntabs=1, .route=r),
        ROW("overlap",     .nkinds=2, .kinds=KOVL, .tabs=T, .ntabs=1, .route=r),
        ROW("past-end",    .nkinds=1, .kinds=KEND, .tabs=T, .ntabs=1, .route=r),
        ROW("past-end-4mb",.nkinds=1, .kinds=KEND4,.tabs=T, .ntabs=1, .route=r,
            .image_size=4u*1024u*1024u),
        ROW("zero-slots",  .nkinds=1, .kinds=KZERO,.tabs=T, .ntabs=1, .route=r),
        ROW("wipe-ghost",  .nkinds=1, .kinds=K1,   .tabs=T, .ntabs=1, .route=r, .nwipe=1, .wipe=W9),
        ROW("neg-count",   .nkinds=-1, .tabs=T, .ntabs=1),
        /* Spelled out rather than built with ROW, because ROW is what supplies
         * the shortname. Otherwise a correct row, so only that check fires. */
        { .name = "no-shortname", .title = "no-shortname", .reset_vec = 0,
          .nkinds = 1, .kinds = K1, .tabs = T, .ntabs = 1, .route = r },
    };
    int caught = 0, n = (int)(sizeof bad / sizeof bad[0]);
    for (int i = 0; i < n; i++)
        if (!dlc_device_check(&bad[i], stdout)) caught++;
        else printf("[MISS] %s passed and should not have\n", bad[i].name);
    /* negative control: a row that is actually fine must pass */
    const DlcDevice good = ROW("good", .nkinds=1, .kinds=K1, .tabs=T, .ntabs=1,
                               .route=r, .nwipe=1, .wipe=W1);
    int goodok = dlc_device_check(&good, stdout);
    printf("\ncaught %d of %d broken rows; good row %s\n",
           caught, n, goodok ? "passes" : "FAILS (control is broken)");
    return (caught == n && goodok) ? 0 : 1;
}
