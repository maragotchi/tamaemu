#include "emu.h"
#include <errno.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define SAVE_SEP '\\'
#define SAVE_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define SAVE_SEP '/'
#define SAVE_MKDIR(p) mkdir((p), 0777)
#endif

static const char *filename_part(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    return slash ? slash + 1 : path;
}

int savepath_default(char *out, size_t outsz, const char *rompath,
                     const DeviceProfile *dev)
{
    if (!out || !outsz || !rompath || !dev || !dev->name) return 0;
    const char *filename = filename_part(rompath);
    size_t parent_len = (size_t)(filename - rompath);
    int n = snprintf(out, outsz, "%.*ssaves%ctamagotchi_%s%c%s%csave.sav",
                     (int)parent_len, rompath, SAVE_SEP, dev->name, SAVE_SEP,
                     filename, SAVE_SEP);
    return n >= 0 && (size_t)n < outsz;
}

int savepath_legacy_default(char *out, size_t outsz, const char *rompath,
                            const DeviceProfile *dev)
{
    const char *filename = filename_part(rompath);
    size_t parent_len = (size_t)(filename - rompath);
    int n = snprintf(out, outsz, "%.*ssaves%ctamagotchi_%s%c%s.sav",
                     (int)parent_len, rompath, SAVE_SEP, dev->name, SAVE_SEP, filename);
    return n >= 0 && (size_t)n < outsz;
}

static int make_dir(const char *path)
{
    return SAVE_MKDIR(path) == 0 || errno == EEXIST;
}

int savepath_mkdirs(const char *rompath, const DeviceProfile *dev)
{
    char path[1024];
    if (!rompath || !dev || !dev->name) return 0;
    const char *filename = filename_part(rompath);
    size_t parent_len = (size_t)(filename - rompath);
    int n = snprintf(path, sizeof path, "%.*ssaves", (int)parent_len, rompath);
    if (n < 0 || (size_t)n >= sizeof path || !make_dir(path)) return 0;
    n = snprintf(path, sizeof path, "%.*ssaves%ctamagotchi_%s",
                 (int)parent_len, rompath, SAVE_SEP, dev->name);
    if (n < 0 || (size_t)n >= sizeof path || !make_dir(path)) return 0;
    n = snprintf(path, sizeof path, "%.*ssaves%ctamagotchi_%s%c%s",
                 (int)parent_len, rompath, SAVE_SEP, dev->name, SAVE_SEP, filename);
    return n >= 0 && (size_t)n < sizeof path && make_dir(path);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int move_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

/* Move the old flat bundle only when this ROM has not already created the new
 * folder layout. Prevents overwrites for older saves. */
int savepath_migrate_legacy(const char *legacy_sav, const char *savpath)
{
    static const char *suffixes[] = { "", ".ram", ".state", ".bak" };
    char oldpath[1100], newpath[1100];
    if (!legacy_sav || !savpath || file_exists(savpath) || !file_exists(legacy_sav))
        return 1;
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        int on = snprintf(oldpath, sizeof oldpath, "%s%s", legacy_sav, suffixes[i]);
        int nn = snprintf(newpath, sizeof newpath, "%s%s", savpath, suffixes[i]);
        if (on < 0 || nn < 0 || (size_t)on >= sizeof oldpath || (size_t)nn >= sizeof newpath)
            return 0;
        if (file_exists(newpath)) return 0;
    }
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        snprintf(oldpath, sizeof oldpath, "%s%s", legacy_sav, suffixes[i]);
        snprintf(newpath, sizeof newpath, "%s%s", savpath, suffixes[i]);
        if (file_exists(oldpath) && !move_file(oldpath, newpath)) return 0;
    }
    return 1;
}
