#include "logcap.h"

#include <stdio.h>
#include <string.h>

static int make_rotated(const wchar_t *base, unsigned n, wchar_t *out, size_t cap)
{
    int r = _snwprintf(out, cap, L"%ls.%u", base, n);
    return r >= 0 && (size_t)r < cap;
}

static int rotate_files(LogCap *log)
{
    wchar_t from[MAX_PATH], to[MAX_PATH];

    if (log->retained > 0) {
        if (!make_rotated(log->base, log->retained, from, MAX_PATH)) return -1;
        DeleteFileW(from);
        for (unsigned n = log->retained; n > 1; n--) {
            if (!make_rotated(log->base, n - 1, from, MAX_PATH) ||
                !make_rotated(log->base, n, to, MAX_PATH)) return -1;
            MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
        }
        if (!make_rotated(log->base, 1, to, MAX_PATH)) return -1;
        if (!MoveFileExW(log->base, to, MOVEFILE_REPLACE_EXISTING)) return -1;
    }
    return 0;
}

static int open_current(LogCap *log)
{
    log->file = CreateFileW(log->base, GENERIC_WRITE, FILE_SHARE_READ,
                            NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log->file == INVALID_HANDLE_VALUE) {
        log->file = NULL;
        return -1;
    }
    log->size = 0;
    return 0;
}

static int rotate_current(LogCap *log)
{
    if (log->file) {
        CloseHandle(log->file);
        log->file = NULL;
    }
    if (rotate_files(log) != 0 || open_current(log) != 0) return -1;
    return 0;
}

int logcap_open(LogCap *log, const wchar_t *base, unsigned long long limit,
                unsigned retained)
{
    if (!log || !base || !*base || limit == 0) return -1;
    memset(log, 0, sizeof *log);
    log->file = NULL;
    log->limit = limit;
    log->retained = retained;
    if (wcsncpy_s(log->base, MAX_PATH, base, _TRUNCATE) != 0) return -1;

    /* Check ownership before changing any retained files. */
    if (GetFileAttributesW(log->base) != INVALID_FILE_ATTRIBUTES) {
        HANDLE existing = CreateFileW(log->base, GENERIC_WRITE, FILE_SHARE_READ,
                                      NULL, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
        if (existing == INVALID_HANDLE_VALUE) return -1;
        CloseHandle(existing);
        if (rotate_files(log) != 0) return -1;
    }
    return open_current(log);
}

int logcap_write(LogCap *log, const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    if (!log || !log->file || (!data && size)) return -1;
    while (size) {
        unsigned long long room = log->limit - log->size;
        size_t chunk = size < room ? size : (size_t)room;
        DWORD written = 0;
        if (chunk && (!WriteFile(log->file, p, (DWORD)chunk, &written, NULL) ||
                      written != (DWORD)chunk)) return -1;
        p += chunk;
        size -= chunk;
        log->size += written;
        if (size && log->size == log->limit && rotate_current(log) != 0) return -1;
    }
    return 0;
}

void logcap_close(LogCap *log)
{
    if (log && log->file) {
        CloseHandle(log->file);
        log->file = NULL;
    }
}
