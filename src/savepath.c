#include "emu.h"
#include <errno.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
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
    return n >= 0 && (size_t)n < sizeof path && make_dir(path);
}
