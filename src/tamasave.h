#ifndef TAMASAVE_H
#define TAMASAVE_H
#include <stddef.h>
#include <stdint.h>

typedef struct TamaSave {
    uint8_t lineage[16];
    uint64_t revision, saved_utc_ms;
    char device[32];
    uint8_t *sav, *ram, *state;
    size_t sav_len, ram_len, state_len;
    char state_runtime[16], state_build[32];
    uint32_t state_version;
} TamaSave;
int tamasave_encode(const TamaSave *, uint8_t **, size_t *, char *, size_t);
int tamasave_decode(const uint8_t *, size_t, TamaSave *, char *, size_t);
void tamasave_free(TamaSave *);
#endif
