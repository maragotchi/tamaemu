#ifndef DESKTOP_TAMASAVE_H
#define DESKTOP_TAMASAVE_H

#include <stddef.h>
#include <stdint.h>
#include "emu.h"

/* Write a freshly-created desktop session. This is the only container API that
 * mints a META revision; --export-tamasave copies the current container. */
int desktop_write_tamasave(const char *savpath, const char *out,
                           const char *device,
                           const uint8_t *sav, size_t sav_len,
                           const uint8_t *ram, size_t ram_len,
                           const uint8_t *state, size_t state_len,
                           const char *state_build,
                           char *why, size_t whysz);

int desktop_export_tamasave(const char *savpath, const char *out,
                            const char *device, char *why, size_t whysz);
int desktop_import_tamasave(const char *in, const char *savpath,
                            const char *device, size_t flash_len, int force,
                            char *why, size_t whysz);

/* 1=container decoded, 0=absent, -1=unreadable/corrupt. STAT validation stays in state.c. */
int desktop_restore_tamasave(const char *savpath, Emu *e, const char *build_id,
                             int restore_state, StateResult *state_result,
                             char *why, size_t whysz);

#endif
