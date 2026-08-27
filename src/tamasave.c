#include "tamasave.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEADER_SIZE = 16, RECORD_HEADER_SIZE = 12, META_SIZE = 32,
       STAT_HEADER_SIZE = 52 };

static void tamasave_why(char *why, size_t whysz, const char *format, ...)
{
    va_list args;
    if (!why || !whysz) return;
    va_start(args, format);
    vsnprintf(why, whysz, format, args);
    va_end(args);
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t value = ~0u;
    while (length--) {
        value ^= *data++;
        for (int bit = 0; bit < 8; bit++)
            value = (value >> 1) ^ (0xedb88320u & -(int)(value & 1));
    }
    return ~value;
}

static void write_u32(uint8_t *out, uint32_t value)
{
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t read_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 |
           (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static void write_u64(uint8_t *out, uint64_t value)
{
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(value >> (8 * i));
}

static uint64_t read_u64(const uint8_t *in)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value |= (uint64_t)in[i] << (8 * i);
    return value;
}

static int add_size(size_t *total, size_t add)
{
    if (add > SIZE_MAX - *total) return 0;
    *total += add;
    return 1;
}

static int valid_device(const char *device, size_t length)
{
    if (!length || length >= 32) return 0;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)device[i] < 0x20 || (unsigned char)device[i] > 0x7e)
            return 0;
    return 1;
}

static int valid_tag(const uint8_t *tag, size_t size)
{
    size_t length = 0;

    while (length < size && tag[length]) length++;
    return length && length < size && valid_device((const char *)tag, length);
}

static int add_record(uint8_t **out, size_t *left, const char type[4],
                      const uint8_t *data, size_t length)
{
    if (length > UINT32_MAX || *left < RECORD_HEADER_SIZE ||
        length > *left - RECORD_HEADER_SIZE) return 0;
    memcpy(*out, type, 4);
    write_u32(*out + 4, (uint32_t)length);
    write_u32(*out + 8, crc32(data, length));
    memcpy(*out + RECORD_HEADER_SIZE, data, length);
    *out += RECORD_HEADER_SIZE + length;
    *left -= RECORD_HEADER_SIZE + length;
    return 1;
}

static int copy_record(uint8_t **out, size_t *out_length,
                       const uint8_t *data, size_t length)
{
    *out = malloc(length);
    if (!*out) return 0;
    memcpy(*out, data, length);
    *out_length = length;
    return 1;
}

void tamasave_free(TamaSave *save)
{
    if (!save) return;
    free(save->sav);
    free(save->ram);
    free(save->state);
    memset(save, 0, sizeof *save);
}

int tamasave_encode(const TamaSave *save, uint8_t **wire, size_t *wire_length,
                    char *why, size_t whysz)
{
    uint8_t meta[META_SIZE], *out, *cursor, *stat = NULL;
    size_t device_length, stat_length = 0, total = HEADER_SIZE, left;
    uint32_t record_count = 4;

    if (!save || !wire || !wire_length || !save->sav || !save->sav_len ||
        !save->ram || !save->ram_len) goto missing;
    device_length = strlen(save->device);
    if (!valid_device(save->device, device_length)) {
        tamasave_why(why, whysz, "invalid device record"); return 0;
    }
    if (save->state) {
        if (!save->state_len || !valid_tag((const uint8_t *)save->state_runtime,
                                           sizeof save->state_runtime) ||
            !valid_tag((const uint8_t *)save->state_build,
                       sizeof save->state_build)) {
            tamasave_why(why, whysz, "invalid snapshot record"); return 0;
        }
        stat_length = STAT_HEADER_SIZE + save->state_len;
        if (stat_length < save->state_len) goto too_large;
        stat = malloc(stat_length);
        if (!stat) goto oom;
        memset(stat, 0, STAT_HEADER_SIZE);
        memcpy(stat, save->state_runtime, strlen(save->state_runtime));
        memcpy(stat + 16, save->state_build, strlen(save->state_build));
        write_u32(stat + 48, save->state_version);
        memcpy(stat + STAT_HEADER_SIZE, save->state, save->state_len);
        record_count++;
    }
    if (!add_size(&total, RECORD_HEADER_SIZE + META_SIZE) ||
        !add_size(&total, RECORD_HEADER_SIZE + device_length) ||
        !add_size(&total, RECORD_HEADER_SIZE + save->sav_len) ||
        !add_size(&total, RECORD_HEADER_SIZE + save->ram_len) ||
        (stat && !add_size(&total, RECORD_HEADER_SIZE + stat_length))) goto too_large;
    out = malloc(total);
    if (!out) goto oom;
    memcpy(out, "TAMASAVE", 8);
    write_u32(out + 8, 1);
    write_u32(out + 12, record_count);
    cursor = out + HEADER_SIZE;
    left = total - HEADER_SIZE;
    memcpy(meta, save->lineage, 16);
    write_u64(meta + 16, save->revision);
    write_u64(meta + 24, save->saved_utc_ms);
    if (!add_record(&cursor, &left, "META", meta, sizeof meta) ||
        !add_record(&cursor, &left, "DEV ", (const uint8_t *)save->device, device_length) ||
        !add_record(&cursor, &left, "SAV ", save->sav, save->sav_len) ||
        !add_record(&cursor, &left, "RAM ", save->ram, save->ram_len) ||
        (stat && !add_record(&cursor, &left, "STAT", stat, stat_length))) {
        free(out); goto too_large;
    }
    free(stat);
    *wire = out;
    *wire_length = total;
    return 1;
missing:
    tamasave_why(why, whysz, "missing required record"); return 0;
oom:
    free(stat); tamasave_why(why, whysz, "out of memory"); return 0;
too_large:
    free(stat); tamasave_why(why, whysz, "container too large"); return 0;
}

int tamasave_decode(const uint8_t *wire, size_t wire_length, TamaSave *save,
                    char *why, size_t whysz)
{
    size_t offset = HEADER_SIZE;
    uint32_t record_count;
    int has_meta = 0, has_device = 0, has_sav = 0, has_ram = 0, has_stat = 0;

    if (!wire || !save || wire_length < HEADER_SIZE ||
        memcmp(wire, "TAMASAVE", 8) || read_u32(wire + 8) != 1) {
        tamasave_why(why, whysz, "bad header"); return 0;
    }
    record_count = read_u32(wire + 12);
    memset(save, 0, sizeof *save);
    for (uint32_t record = 0; record < record_count; record++) {
        const uint8_t *data;
        uint32_t length;
        if (offset > wire_length || wire_length - offset < RECORD_HEADER_SIZE) {
            tamasave_why(why, whysz, "truncated record"); goto bad;
        }
        length = read_u32(wire + offset + 4);
        if (length > wire_length - offset - RECORD_HEADER_SIZE) {
            tamasave_why(why, whysz, "bad record"); goto bad;
        }
        /* A STAT snapshot is optional. Its framing tags identify it across runtimes;
         * desktop payload compatibility is owned solely by TAMASTAT in state.c.
         * A malformed or stale STAT must never keep SAV and RAM from opening. */
        if (read_u32(wire + offset + 8) !=
            crc32(wire + offset + RECORD_HEADER_SIZE, length)) {
            if (!memcmp(wire + offset, "STAT", 4)) {
                tamasave_why(why, whysz, "snapshot ignored");
                offset += RECORD_HEADER_SIZE + length;
                continue;
            }
            tamasave_why(why, whysz, "bad record"); goto bad;
        }
        data = wire + offset + RECORD_HEADER_SIZE;
        if (!memcmp(wire + offset, "META", 4) && length == META_SIZE && !has_meta) {
            memcpy(save->lineage, data, 16);
            save->revision = read_u64(data + 16);
            save->saved_utc_ms = read_u64(data + 24);
            has_meta = 1;
        } else if (!memcmp(wire + offset, "DEV ", 4) && valid_device((const char *)data, length) && !has_device) {
            memcpy(save->device, data, length); has_device = 1;
        } else if (!memcmp(wire + offset, "SAV ", 4) && length && !has_sav &&
                   copy_record(&save->sav, &save->sav_len, data, length)) {
            has_sav = 1;
        } else if (!memcmp(wire + offset, "RAM ", 4) && length && !has_ram &&
                   copy_record(&save->ram, &save->ram_len, data, length)) {
            has_ram = 1;
        } else if (!memcmp(wire + offset, "STAT", 4)) {
            if (!has_stat && length > STAT_HEADER_SIZE && valid_tag(data, 16) &&
                valid_tag(data + 16, 32) &&
                copy_record(&save->state, &save->state_len, data + STAT_HEADER_SIZE,
                            length - STAT_HEADER_SIZE)) {
                memcpy(save->state_runtime, data, 16);
                memcpy(save->state_build, data + 16, 32);
                save->state_version = read_u32(data + 48);
                has_stat = 1;
            } else {
                /* A bad optional snapshot still leaves SAV and RAM usable. */
                tamasave_why(why, whysz, "snapshot ignored");
            }
        } else if (memcmp(wire + offset, "META", 4) && memcmp(wire + offset, "DEV ", 4) &&
                   memcmp(wire + offset, "SAV ", 4) && memcmp(wire + offset, "RAM ", 4) &&
                   memcmp(wire + offset, "STAT", 4)) {
            /* Future records are safe to skip after their checksum validated. */
        } else {
            tamasave_why(why, whysz, "duplicate or invalid record"); goto bad;
        }
        offset += RECORD_HEADER_SIZE + length;
    }
    if (offset != wire_length || !has_meta || !has_device || !has_sav || !has_ram) {
        tamasave_why(why, whysz, "missing required record"); goto bad;
    }
    return 1;
bad:
    tamasave_free(save);
    return 0;
}
