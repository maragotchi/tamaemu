#include "launcher_crosssave.h"
#include "tamasave.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void crosssave_why(wchar_t *why, size_t whysz, const wchar_t *text)
{
    if (!why || !whysz) return;
    wcsncpy(why, text, whysz - 1);
    why[whysz - 1] = L'\0';
}

static int read_file(const wchar_t *path, uint8_t **out, size_t *outsz,
                     wchar_t *why, size_t whysz)
{
    FILE *f = _wfopen(path, L"rb");
    long long length;
    uint8_t *wire;

    if (!f) { crosssave_why(why, whysz, L"cannot open the cross save"); return 0; }
    if (_fseeki64(f, 0, SEEK_END) != 0 || (length = _ftelli64(f)) <= 0 ||
        (uint64_t)length > SIZE_MAX) {
        fclose(f); crosssave_why(why, whysz, L"cannot read the cross save"); return 0;
    }
    rewind(f);
    wire = malloc((size_t)length);
    if (!wire || fread(wire, 1, (size_t)length, f) != (size_t)length) {
        free(wire); fclose(f); crosssave_why(why, whysz, L"cannot read the cross save"); return 0;
    }
    fclose(f);
    *out = wire;
    *outsz = (size_t)length;
    return 1;
}

int crosssave_read_identity(const wchar_t *path, CrossSaveIdentity *out,
                            wchar_t *why, size_t whysz)
{
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    TamaSave save = {0};
    char decode_why[160];

    if (!path || !out || !read_file(path, &wire, &wire_len, why, whysz)) return 0;
    if (!tamasave_decode(wire, wire_len, &save, decode_why, sizeof decode_why)) {
        free(wire); crosssave_why(why, whysz, L"not a readable cross save"); return 0;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->device, save.device, sizeof out->device);
    memcpy(out->lineage, save.lineage, sizeof out->lineage);
    out->revision = save.revision;
    tamasave_free(&save);
    free(wire);
    return 1;
}

static int path_exists(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int has_suffix(const wchar_t *path, const wchar_t *suffix)
{
    size_t pathlen = wcslen(path), suffixlen = wcslen(suffix);
    return pathlen >= suffixlen && !_wcsicmp(path + pathlen - suffixlen, suffix);
}

static int destination_for(const wchar_t *source, const char *device,
                           wchar_t *out, size_t outsz, wchar_t *why, size_t whysz)
{
    const wchar_t *slash, *filename;
    wchar_t wdevice[32];
    size_t parent_len, filename_len;
    int written;

    slash = wcsrchr(source, L'\\');
    {
        const wchar_t *forward = wcsrchr(source, L'/');
        if (!slash || (forward && forward > slash)) slash = forward;
    }
    filename = slash ? slash + 1 : source;
    parent_len = (size_t)(filename - source);
    if (!filename[0] || !has_suffix(filename, L".tamasave") ||
        !MultiByteToWideChar(CP_ACP, 0, device, -1, wdevice,
                             (int)(sizeof wdevice / sizeof wdevice[0]))) {
        crosssave_why(why, whysz, L"invalid cross save name or device"); return 0;
    }
    filename_len = wcslen(filename) - wcslen(L".tamasave");
    /* This deliberately keys the folder by the container basename, just as
     * savepath_default keys a ROM save folder by the ROM basename. */
    written = _snwprintf(out, outsz, L"%.*ls%s%s%s%.*ls%s%s",
                         (int)parent_len, source, L"saves\\tamagotchi_", wdevice,
                         L"\\", (int)filename_len, filename, L"\\", L"save.sav");
    if (written < 0 || (size_t)written >= outsz) {
        crosssave_why(why, whysz, L"derived save path is too long"); return 0;
    }
    return 1;
}

static int companion_path(const wchar_t *sav, wchar_t *out, size_t outsz)
{
    int written = _snwprintf(out, outsz, L"%ls.tamasave", sav);
    return written >= 0 && (size_t)written < outsz;
}

int crosssave_working_sibling(const wchar_t *container, wchar_t *out, size_t outsz)
{
    static const wchar_t suffix[] = L".sav.tamasave";
    size_t length, remove;

    if (!container || !out || !outsz || !has_suffix(container, suffix)) return 0;
    length = wcslen(container);
    remove = wcslen(L".tamasave");
    if (length <= remove || length - remove >= outsz) return 0;
    memcpy(out, container, (length - remove) * sizeof *out);
    out[length - remove] = L'\0';
    return path_exists(out);
}

static int candidate_exists(const wchar_t *sav)
{
    wchar_t container[MAX_PATH * 4];
    return path_exists(sav) || (companion_path(sav, container,
                                                sizeof container / sizeof container[0]) &&
                                path_exists(container));
}

static int sibling_path(const wchar_t *base, unsigned number,
                        wchar_t *out, size_t outsz)
{
    size_t len = wcslen(base);
    const wchar_t *suffix = L".sav";
    size_t suffixlen = 4;
    int written;
    if (len < suffixlen || _wcsicmp(base + len - suffixlen, suffix)) return 0;
    written = _snwprintf(out, outsz, L"%.*ls (%u)%ls", (int)(len - suffixlen),
                         base, number, suffix);
    return written >= 0 && (size_t)written < outsz;
}

int crosssave_resolve_destination(const wchar_t *source,
                                  const CrossSaveIdentity *incoming,
                                  wchar_t *out, size_t outsz,
                                  CrossSaveRoute *route,
                                  wchar_t *why, size_t whysz)
{
    wchar_t existing_path[MAX_PATH * 4];
    CrossSaveIdentity existing;

    if (!source || !incoming || !out || !route ||
        !destination_for(source, incoming->device, out, outsz, why, whysz)) return 0;
    if (!candidate_exists(out)) { *route = CROSSSAVE_IMPORT; return 1; }

    if (companion_path(out, existing_path,
                       sizeof existing_path / sizeof existing_path[0]) &&
        crosssave_read_identity(existing_path, &existing, why, whysz) &&
        !memcmp(existing.lineage, incoming->lineage, sizeof existing.lineage)) {
        *route = incoming->revision > existing.revision ? CROSSSAVE_IMPORT : CROSSSAVE_CONFIRM_STALE;
        return 1;
    }

    for (unsigned number = 2; number < 10000; number++) {
        if (!sibling_path(out, number, existing_path,
                          sizeof existing_path / sizeof existing_path[0])) break;
        if (!candidate_exists(existing_path)) {
            wcsncpy(out, existing_path, outsz - 1); out[outsz - 1] = L'\0';
            *route = CROSSSAVE_FOREIGN_SIBLING;
            return 1;
        }
    }
    crosssave_why(why, whysz, L"cannot choose a free cross save name");
    return 0;
}
