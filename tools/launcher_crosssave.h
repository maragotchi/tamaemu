#ifndef LAUNCHER_CROSSSAVE_H
#define LAUNCHER_CROSSSAVE_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

typedef struct {
    char device[32];
    uint8_t lineage[16];
    uint64_t revision;
} CrossSaveIdentity;

typedef enum {
    CROSSSAVE_IMPORT = 0,
    CROSSSAVE_CONFIRM_STALE,
    CROSSSAVE_FOREIGN_SIBLING
} CrossSaveRoute;

int crosssave_read_identity(const wchar_t *path, CrossSaveIdentity *out,
                            wchar_t *why, size_t whysz);
int crosssave_working_sibling(const wchar_t *container, wchar_t *out, size_t outsz);
int crosssave_resolve_destination(const wchar_t *source,
                                  const CrossSaveIdentity *incoming,
                                  wchar_t *out, size_t outsz,
                                  CrossSaveRoute *route,
                                  wchar_t *why, size_t whysz);

#endif
