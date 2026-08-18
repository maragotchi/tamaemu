#ifndef TAMALOGCAP_H
#define TAMALOGCAP_H

#include <windows.h>
#include <stddef.h>

#define LOGCAP_RETAINED 2

typedef struct LogCap {
    HANDLE file;
    wchar_t base[MAX_PATH];
    unsigned long long limit;
    unsigned retained;
    unsigned long long size;
} LogCap;

int  logcap_open(LogCap *log, const wchar_t *base, unsigned long long limit,
                 unsigned retained);
int  logcap_write(LogCap *log, const void *data, size_t size);
void logcap_close(LogCap *log);

#endif
