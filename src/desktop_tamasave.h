#ifndef DESKTOP_TAMASAVE_H
#define DESKTOP_TAMASAVE_H

#include <stddef.h>

int desktop_export_tamasave(const char *savpath, const char *out,
                            const char *device, char *why, size_t whysz);
int desktop_import_tamasave(const char *in, const char *savpath,
                            const char *device, size_t flash_len, int force,
                            char *why, size_t whysz);

#endif
