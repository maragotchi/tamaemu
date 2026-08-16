
/* Dump the routing-relevant header bytes of every payload under a folder.
 *
 *   hdrscan [--device <name>] <libdir>
 *
 * One TSV line per file: dir, file, type [0x4E], cat [the device's cat_off -
 * 0x4F on the iD, 0x50 everywhere else], flag [0x51], record length, ASCII id
 * [0x34..0x48]. Unreadable payloads get a line too,
 * with an err field - a survey that silently drops files cannot answer "is
 * this folder uniform?", which is the only question it exists for.
 *
 * With --device, two more columns: the kind and label that device's router
 * gives the payload. That turns a dumped catalogue into a routing regression -
 * route every file, compare the kind against what its folder says it is -
 * exercising the very dlc_route() the installer runs, not a re-implementation.
 *
 * It links tools/dlc.c so the container parse (JPEG EOI -> TAMAGO magic ->
 * length at +0x48 -> sum16 MAC) is the same code the installer routes with.
 * Re-implementing it in a scratch script is how you end up surveying bytes at
 * offsets the installer never reads.
 */

#include "dlc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

static const DlcDevice *g_dev;          /* NULL unless --device was given */

static int has_payload_ext(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    char e[8];
    size_t n = 0;
    for (const char *q = d + 1; *q && n + 1 < sizeof e; q++)
        e[n++] = (char)tolower((unsigned char)*q);
    e[n] = '\0';
    return !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "bin");
}

static void emit(const char *dir, const char *name, const char *full)
{
    uint8_t *p = NULL;
    size_t plen = 0;
    char err[DLC_ERR_MAX];
    if (dlc_extract_payload(full, &p, &plen, err, sizeof err) != 0) {
        printf("%s\t%s\t-\t-\t-\t-\t\t%s\n", dir, name, err);
        return;
    }
    int typ = -1;
    char id[DLC_ID_MAX], nm[DLC_NAME_MAX];
    dlc_parse_header(p, plen, &typ, id, sizeof id, nm, sizeof nm);
    /* The category offset is the device's own (0x4F on the iD, 0x50 elsewhere);
     * with no --device the 0x50 default applies. */
    int cat  = (plen > dlc_cat_off(g_dev)) ? p[dlc_cat_off(g_dev)] : -1;
    int flag = (plen > 0x51) ? p[0x51] : -1;
    /* The id field is raw bytes and is not always ASCII (four of the 47 games
     * are Japanese-named). Escape anything unprintable so one odd file cannot
     * corrupt the whole TSV. */
    char safe[4 * DLC_ID_MAX];
    size_t s = 0;
    for (size_t i = 0; id[i] && s + 5 < sizeof safe; i++) {
        unsigned char c = (unsigned char)id[i];
        if (c >= 0x20 && c < 0x7F && c != '\t') safe[s++] = (char)c;
        else s += (size_t)snprintf(safe + s, sizeof safe - s, "\\x%02x", c);
    }
    safe[s] = '\0';
    printf("%s\t%s\t%02x\t%02x\t%02x\t%zu\t%s\t",
           dir, name, typ, cat, flag, plen, safe);
    if (g_dev) {
        const char *label = NULL;
        int kind = -1;
        dlc_route(g_dev, typ, cat, id, flag, p, plen, &label, &kind);
        printf("\t%d\t%s", kind, label ? label : "");
    }
    printf("\n");
    free(p);
}

static void walk(const char *dir, const char *label)
{
    DIR *dh = opendir(dir);
    if (!dh) { fprintf(stderr, "cannot read %s\n", dir); return; }
    struct dirent *e;
    while ((e = readdir(dh)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[DLC_PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char sub[DLC_PATH_MAX];
            if (label[0]) snprintf(sub, sizeof sub, "%s/%s", label, e->d_name);
            else          snprintf(sub, sizeof sub, "%s", e->d_name);
            walk(full, sub);
        } else if (has_payload_ext(e->d_name)) {
            emit(label[0] ? label : ".", e->d_name, full);
        }
    }
    closedir(dh);
}

int main(int argc, char **argv)
{
    int a = 1;
    if (argc >= 3 && !strcmp(argv[1], "--device")) {
        g_dev = dlc_device_find(argv[2]);
        if (!g_dev) {
            fprintf(stderr, "no device named '%s'\n", argv[2]);
            return 2;
        }
        a = 3;
    }
    if (a >= argc) {
        fprintf(stderr, "usage: hdrscan [--device <name>] <libdir>\n");
        return 2;
    }
    printf("dir\tfile\ttype\tcat\tflag\tlen\tid\terr%s\n",
           g_dev ? "\tkind\tlabel" : "");
    for (int i = a; i < argc; i++) walk(argv[i], "");
    return 0;
}
