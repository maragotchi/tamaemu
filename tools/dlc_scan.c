
#include "dlc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

int dlc_tab_count(const DlcDevice *d) { return d ? d->ntabs : 0; }
const char *dlc_tab_at(const DlcDevice *d, int i)
{
    return (d && i >= 0 && i < d->ntabs) ? d->tabs[i] : NULL;
}

/* Unlisted VDP labels use VDP Pierces; other labels use Destinations. */
static const struct { const char *label, *tab; } LABEL_TAB[] = {
    { "game",       "Games"       },
    { "meal",       "Meals"       },
    { "snack",      "Snacks"      },
    { "clothing",   "Clothing"    },
    { "accessory",  "Accessories" },
    { "item",       "Items"       },
    { "wallpaper",  "Wallpapers"  },
    { "daily item", "Daily Items" },
    { "seed",       "Seeds"       },
    /* 4U labels map to its menu names rather than the generic tabs. */
    { "tamamori outfit",    "TamaMori"        },
    { "tamamori accessory", "TamaMori"        },
    { "interior",           "Gotchi Interior" },
    { "minigame",           "Game Center"     },
    { "outing destination", "Door Icon"       },
    { "toy",                "TamaDepa"        },
    { "coupon",             "Coupon"          },
    { "decoration",         "Decorations"     },
    { "bingo",              "Bingo"           },
    { "gashapon",           "Gashapon"        },
    { "character",          "Characters"      },
    /* iD-specific labels whose generic names belong to other device tabs. */
    { "tama depa toy",      "Toys"            },
    { "letter",             "Letters"         },
    { "reform wallpaper",   "Interior"        },
    { "outfit",             "Outfits"         },
    { "backdrop",           "Backdrop"        },
    { "outing",             "Destinations"    },
};

static int has_ext(const char *name, const char *ext)
{
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    char e[8];
    size_t n = 0;
    for (const char *q = d + 1; *q && n + 1 < sizeof e; q++)
        e[n++] = (char)tolower((unsigned char)*q);
    e[n] = '\0';
    return !strcmp(e, ext);
}

static int has_payload_ext(const char *name)
{
    return has_ext(name, "jpg") || has_ext(name, "jpeg") || has_ext(name, "bin");
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* qsort takes no context argument, and qsort_s is not portable here. Set once
 * in dlc_scan_library() before the sort; the scan is not re-entrant anyway. */
static const DlcDevice *g_sort_dev;

static int cmp_item(const void *a, const void *b)
{
    const DlcItem *x = a, *y = b;
    int tx = 0, ty = 0;
    const DlcDevice *d = g_sort_dev;
    for (int i = 0; d && i < d->ntabs; i++) {
        if (!strcmp(x->tab, d->tabs[i])) tx = i;
        if (!strcmp(y->tab, d->tabs[i])) ty = i;
    }
    if (tx != ty) return tx - ty;
    const char *p = x->display, *q = y->display;
    while (*p && *q) {
        int cp = tolower((unsigned char)*p), cq = tolower((unsigned char)*q);
        if (cp != cq) return cp - cq;
        p++; q++;
    }
    return (int)((unsigned char)*p) - (int)((unsigned char)*q);
}

static void stem_of(const char *path, char *out, size_t outsz)
{
    const char *b = path;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    const char *dot = strrchr(b, '.');
    size_t n = dot ? (size_t)(dot - b) : strlen(b);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, b, n);
    out[n] = '\0';
}

static void base_of(const char *path, char *out, size_t outsz)
{
    const char *b = path;
    size_t l = strlen(path);
    while (l > 0 && (path[l-1] == '/' || path[l-1] == '\\')) l--;
    for (size_t i = 0; i < l; i++)
        if (path[i] == '/' || path[i] == '\\') b = path + i + 1;
    size_t n = (size_t)(path + l - b);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, b, n);
    out[n] = '\0';
}

/* Extract friendly names shaped as DL_<tag>_<name>.jpg or .did. */
static void dl_name_of(const char *nm, char *out, size_t outsz)
{
    if (strncmp(nm, "DL_", 3) != 0) return;
    const char *inner = strchr(nm + 3, '_');
    if (!inner || !*++inner) return;
    size_t n = strlen(inner);
    if (n <= 4) return;
    if (strcmp(inner + n - 4, ".jpg") != 0 &&
        strcmp(inner + n - 4, ".did") != 0) return;
    n -= 4;
    if (n == 0 || n >= outsz) return;
    memcpy(out, inner, n);
    out[n] = '\0';
}

/* Choose a tab and display name. Unreadable or unroutable payloads return an
 * empty tab and callers omit them. */
void dlc_tab_for(const DlcDevice *d, const char *path, char *tab, size_t tabsz,
                 char *display, size_t dispsz)
{
    stem_of(path, display, dispsz);
    tab[0] = '\0';

    uint8_t *p = NULL;
    size_t plen = 0;
    char err[DLC_ERR_MAX];
    if (dlc_extract_payload(path, &p, &plen, err, sizeof err) != 0)
        return;

    int typ = -1;
    char id[DLC_ID_MAX], nm[DLC_NAME_MAX];
    dlc_parse_header(p, plen, &typ, id, sizeof id, nm, sizeof nm);
    /* 4U records prefer community English names, then header names. */
    if (d && strncmp(d->name, "4u", 2) == 0) {
        dl_name_of(nm, display, dispsz);
        const char *en = dlc_4u_name_en(id);
        if (en) snprintf(display, dispsz, "%s", en);
    }
    const char *label = NULL;
    int kind = -1;
    dlc_route(d, typ, (plen > dlc_cat_off(d)) ? p[dlc_cat_off(d)] : -1, id,
              (plen > 0x51) ? p[0x51] : 0, p, plen, &label, &kind);
    free(p);

    if (!label || kind < 0) return;
    for (size_t i = 0; i < sizeof LABEL_TAB / sizeof LABEL_TAB[0]; i++)
        if (!strcmp(label, LABEL_TAB[i].label)) {
            snprintf(tab, tabsz, "%s", LABEL_TAB[i].tab);
            return;
        }
    if (strstr(label, "VDP")) snprintf(tab, tabsz, "VDP Pierces");
    else                      snprintf(tab, tabsz, "Destinations");
}

/* A VDP set is a folder containing 2-4 DecoPierceLoader parts. */
static int is_vdp_set(char **paths, int n)
{
    if (n < 2 || n > VDP_PARTS_MAX) return 0;
    for (int i = 0; i < n; i++) {
        uint8_t *p = NULL;
        size_t plen = 0;
        char err[DLC_ERR_MAX];
        if (dlc_extract_payload(paths[i], &p, &plen, err, sizeof err) != 0)
            return 0;
        int typ = -1;
        char id[DLC_ID_MAX], nm[DLC_NAME_MAX];
        dlc_parse_header(p, plen, &typ, id, sizeof id, nm, sizeof nm);
        free(p);
        if (strncmp(nm, "DecoPierceLoader", 16) != 0) return 0;
    }
    return 1;
}

static int device_has_tab(const DlcDevice *d, const char *tab)
{
    for (int i = 0; d && i < d->ntabs; i++)
        if (!strcmp(d->tabs[i], tab)) return 1;
    return 0;
}

typedef struct { DlcItem *v; int n, cap; } ItemVec;
typedef struct { int entries; int limited; } ScanBudget;

static void vec_push(ItemVec *iv, const DlcItem *it)
{
    if (iv->n == iv->cap) {
        iv->cap = iv->cap ? iv->cap * 2 : 64;
        iv->v = realloc(iv->v, (size_t)iv->cap * sizeof *iv->v);
    }
    iv->v[iv->n++] = *it;
}

/* A recognized VDP set becomes one row, so do not scan its parts again. */
static void walk(const DlcDevice *d, const char *dir, ItemVec *iv,
                 ScanBudget *budget)
{
    if (budget->entries >= DLC_SCAN_MAX_ENTRIES) {
        budget->limited = 1;
        return;
    }
    DIR *dh = opendir(dir);
    if (!dh) return;

    char **files = NULL;
    int nf = 0, capf = 0;
    char **subs = NULL;
    int ns = 0, caps = 0;
    struct dirent *e;
    while ((e = readdir(dh)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (budget->entries++ >= DLC_SCAN_MAX_ENTRIES) {
            budget->limited = 1;
            break;
        }
        char full[DLC_PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (ns == caps) { caps = caps ? caps * 2 : 16;
                              subs = realloc(subs, (size_t)caps * sizeof *subs); }
            subs[ns++] = strdup(full);
        } else if (has_payload_ext(e->d_name)) {
            if (nf == capf) { capf = capf ? capf * 2 : 16;
                              files = realloc(files, (size_t)capf * sizeof *files); }
            files[nf++] = strdup(full);
        }
    }
    closedir(dh);

    qsort(files, (size_t)nf, sizeof *files, cmp_str);
    qsort(subs,  (size_t)ns, sizeof *subs,  cmp_str);

    int pruned = 0;
    if (nf > 0) {
        if (device_has_tab(d, "VDP Pierces") && is_vdp_set(files, nf)) {
            DlcItem it;
            memset(&it, 0, sizeof it);
            char b[256];
            base_of(dir, b, sizeof b);
            /* Leave room for the "(N parts)" suffix. */
            snprintf(it.display, sizeof it.display, "%.230s  (%d parts)", b, nf);
            snprintf(it.tab, sizeof it.tab, "VDP Pierces");
            it.nparts = nf;
            for (int i = 0; i < nf; i++)
                snprintf(it.parts[i], DLC_PATH_MAX, "%s", files[i]);
            vec_push(iv, &it);
            pruned = 1;
        } else {
            for (int i = 0; i < nf; i++) {
                DlcItem it;
                memset(&it, 0, sizeof it);
                dlc_tab_for(d, files[i], it.tab, sizeof it.tab,
                            it.display, sizeof it.display);
                if (!it.tab[0]) continue;
                it.nparts = 1;
                snprintf(it.parts[0], DLC_PATH_MAX, "%s", files[i]);
                vec_push(iv, &it);
            }
        }
    }

    if (!pruned)
        for (int i = 0; i < ns; i++) walk(d, subs[i], iv, budget);

    for (int i = 0; i < nf; i++) free(files[i]);
    for (int i = 0; i < ns; i++) free(subs[i]);
    free(files);
    free(subs);
}

/* Hash extracted records so wrapped and bare copies match; 0 means unreadable. */
static unsigned long long record_hash(const char *path)
{
    uint8_t *p = NULL;
    size_t plen = 0;
    char err[DLC_ERR_MAX];
    if (dlc_extract_payload(path, &p, &plen, err, sizeof err) != 0) return 0;
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < plen; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    free(p);
    return h ? h : 1;
}

/* Match card members shaped as <base-ending-in-digit>_<part>. */
static int looks_like_card_member(const char *path)
{
    const char *leaf = path;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') leaf = q + 1;
    const char *dot = strrchr(leaf, '.');
    size_t stem = dot ? (size_t)(dot - leaf) : strlen(leaf);
    size_t us = stem;
    while (us > 0 && leaf[us-1] >= '0' && leaf[us-1] <= '9') us--;
    if (us == 0 || us == stem || leaf[us-1] != '_') return 0;
    us--;
    return us > 0 && leaf[us-1] >= '0' && leaf[us-1] <= '9';
}

/* Keep one copy of each record. Prefer a card member so sets remain complete;
 * otherwise prefer a bare .bin over a wrapper. */
static void drop_duplicate_records(ItemVec *iv)
{
    if (iv->n < 2) return;
    unsigned long long *hash = malloc((size_t)iv->n * sizeof *hash);
    if (!hash) return;
    for (int i = 0; i < iv->n; i++)
        hash[i] = (iv->v[i].nparts == 1) ? record_hash(iv->v[i].parts[0]) : 0;

    int w = 0;
    for (int i = 0; i < iv->n; i++) {
        int keep = 1;
        if (hash[i]) {
            for (int j = 0; j < w; j++) {
                if (hash[j] != hash[i]) continue;
                keep = 0;
                int jcard = looks_like_card_member(iv->v[j].parts[0]);
                int icard = looks_like_card_member(iv->v[i].parts[0]);
                int better = (icard && !jcard) ||
                             (icard == jcard &&
                              !has_ext(iv->v[j].parts[0], "bin") &&
                               has_ext(iv->v[i].parts[0], "bin"));
                if (better) {
                    unsigned long long th = hash[j];
                    iv->v[j] = iv->v[i];
                    hash[j] = th;
                }
                break;
            }
        }
        if (keep) {
            if (w != i) { iv->v[w] = iv->v[i]; hash[w] = hash[i]; }
            w++;
        }
    }
    iv->n = w;
    free(hash);
}

int dlc_scan_library(const DlcDevice *d, const char *libdir,
                     DlcItem **out, int *count)
{
    *out = NULL; *count = 0;
    struct stat st;
    if (stat(libdir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;

    ItemVec iv = { NULL, 0, 0 };
    ScanBudget budget = { 0, 0 };
    walk(d, libdir, &iv, &budget);
    g_sort_dev = d;
    qsort(iv.v, (size_t)iv.n, sizeof *iv.v, cmp_item);
    drop_duplicate_records(&iv);
    *out = iv.v; *count = iv.n;
    return 0;
}
