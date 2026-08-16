
/* Command-line front end for dlc.c, and the harness the port is verified with.
 *
 *   dlcctl [--device NAME] <target.sav> <listfile> [--wipe] [--backup]
 *   dlcctl [--device NAME] --scan <libdir>
 *
 * --device defaults to "ps". Names come from the same namespace as the
 * emulator's --device, but this is the narrower table: a machine appears here
 * only if it has a content layer to install into, so the Plus Color - memory
 * map only - is absent.
 *
 * Payload paths come from a file, one per line, because the library runs to
 * hundreds of items and blows past the command-line length limit.
 *
 * Output is deliberately the same normalised table the Python driver emits
 * (file|type|id|name|label|kind|off|error, then USAGE lines) so the two can be
 * diffed directly against another device-tool run.
 */

#include "dlc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *target = NULL, *listfile = NULL;
    int wipe = 0, backup = 0;
    const DlcDevice *dev = dlc_device_default();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wipe"))        wipe = 1;
        else if (!strcmp(argv[i], "--backup")) backup = 1;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            if (!strcmp(argv[i + 1], "help") || !strcmp(argv[i + 1], "list")) {
                fprintf(stderr, "known devices:\n");
                for (int k = 0; k < dlc_device_count(); k++)
                    fprintf(stderr, "  %-12s %s\n",
                            dlc_device_at(k)->name, dlc_device_at(k)->title);
                return 0;
            }
            dev = dlc_device_find(argv[++i]);
            if (!dev) {
                fprintf(stderr, "unknown device '%s'\nknown devices:\n", argv[i]);
                for (int k = 0; k < dlc_device_count(); k++)
                    fprintf(stderr, "  %-12s %s\n",
                            dlc_device_at(k)->name, dlc_device_at(k)->title);
                return 2;
            }
        }
        else if (!target)                      target = argv[i];
        else if (!listfile)                    listfile = argv[i];
    }
    /* Every row, not just the selected one: the table is edited by hand and a
     * broken row is a broken build, so the tool that writes saves should not
     * start on one. Nothing here depends on the command line. */
    for (int k = 0; k < dlc_device_count(); k++)
        if (!dlc_device_check(dlc_device_at(k), stderr)) return 3;

    /* --scan <libdir>: dump the grouped library the way the installer will show
     * it, so it can be diffed against scan_library() in launcher.py. */
    if (target && !strcmp(target, "--scan")) {
        if (!listfile) { fprintf(stderr, "usage: dlcctl [--device NAME] --scan <libdir>\n"); return 2; }
        DlcItem *items = NULL;
        int n = 0;
        if (dlc_scan_library(dev, listfile, &items, &n) != 0) {
            fprintf(stderr, "cannot read %s\n", listfile);
            return 1;
        }
        for (int i = 0; i < n; i++) {
            printf("%s|%s|%d", items[i].tab, items[i].display, items[i].nparts);
            for (int j = 0; j < items[i].nparts; j++)
                printf("|%s", items[i].parts[j]);
            printf("\n");
        }
        free(items);
        return 0;
    }

    /* --slots <sav> <kind>: what is installed in one store, one row per slot.
     * Same shape as the USAGE lines, so it can be diffed in a shell test. */
    if (target && !strcmp(target, "--slots")) {
        if (!listfile) { fprintf(stderr, "usage: dlcctl [--device NAME] --slots <sav> <kind>\n"); return 2; }
        /* kind is the third positional argument; find it by re-walking argv
         * rather than trusting argc, since --device consumes two slots. */
        int kind = 7;
        int pos = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--wipe") || !strcmp(argv[i], "--backup")) continue;
            if (!strcmp(argv[i], "--device")) { i++; continue; }
            pos++;
            if (pos == 3) { kind = (int)strtol(argv[i], 0, 0); break; }
        }
        FILE *f = fopen(listfile, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", listfile); return 1; }
        uint32_t imgsz = dlc_image_size(dev);
        uint8_t *img = malloc(imgsz);
        if (!img || fread(img, 1, imgsz, f) != imgsz) {
            fprintf(stderr, "%s is not a %u MB image\n", listfile, imgsz >> 20);
            free(img); fclose(f); return 1;
        }
        fclose(f);
        DlcSlot s[64];
        int n = dlc_store_slots(dev, img, kind, s, 64);
        for (int i = 0; i < n; i++)
            printf("SLOT %d %s %08lx %s\n", s[i].slot,
                   s[i].occupied ? "used" : "free", s[i].off,
                   s[i].occupied ? s[i].id : "-");
        free(img);
        return 0;
    }

    if (!target || !listfile) {
        fprintf(stderr, "usage: dlcctl [--device NAME] <target.sav> <listfile> [--wipe] [--backup]\n");
        fprintf(stderr, "       dlcctl [--device NAME] --scan <libdir>\n");
        fprintf(stderr, "       dlcctl [--device NAME] --slots <sav> <kind>\n");
        return 2;
    }

    FILE *lf = fopen(listfile, "rb");
    if (!lf) { fprintf(stderr, "cannot open %s\n", listfile); return 2; }

    int cap = 64, n = 0;
    char **paths = malloc((size_t)cap * sizeof *paths);
    char line[1024];
    while (fgets(line, sizeof line, lf)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (!l) continue;
        if (n == cap) { cap *= 2; paths = realloc(paths, (size_t)cap * sizeof *paths); }
        paths[n] = malloc(l + 1);
        memcpy(paths[n], line, l + 1);
        n++;
    }
    fclose(lf);

    DlcResult *res = calloc((size_t)(n > 0 ? n : 1), sizeof *res);
    char err[256] = "";
    /* No frees: the CLI never replaces an occupied slot, only --wipe clears. */
    if (dlc_inject(dev, target, (const char *const *)paths, n, wipe, backup,
                   NULL, 0, res, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    for (int i = 0; i < n; i++) {
        DlcResult *r = &res[i];
        char typ[16], kind[16], off[24];
        if (r->type < 0) snprintf(typ, sizeof typ, "-");
        else             snprintf(typ, sizeof typ, "%02x", r->type);
        if (r->kind < 0) snprintf(kind, sizeof kind, "-");
        else             snprintf(kind, sizeof kind, "%d", r->kind);
        if (r->off < 0)  snprintf(off, sizeof off, "-");
        else             snprintf(off, sizeof off, "%08lx", r->off);
        printf("%s|%s|%s|%s|%s|%s|%s|%s\n",
               r->file, typ, r->id, r->name,
               r->label ? r->label : "", kind, off, r->error);
    }

    /* usage summary, read back from the written image */
    FILE *f = fopen(target, "rb");
    if (f) {
        uint32_t imgsz = dlc_image_size(dev);
        uint8_t *img = malloc(imgsz);
        if (img && fread(img, 1, imgsz, f) == imgsz) {
            DlcUsage u[16];
            int un = dlc_store_usage(dev, img, u, 16);
            for (int i = 0; i < un; i++)
                printf("USAGE %s %d/%d\n", u[i].label, u[i].used, u[i].max);
        }
        free(img);
        fclose(f);
    }

    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
    free(res);
    return 0;
}
