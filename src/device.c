
/* Device selection is explicit because ROM size alone cannot identify a map, and this is just easier than assuming and breaking things. */
#include "emu.h"
#include <string.h>

static const DeviceProfile devices[] = {
    {
        .name = "ps",
        .title = "Tamagotchi P's",
        .rom_base   = PS_ROM_BASE,    .rom_size    = PS_ROM_SIZE,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        .ir_mode_flag = 0x00000DB3u,  /* 2 = visit/connect, 1 = item exchange */
        .has_sleep_flag = true,
        .sleep_flag   = 0x0000146Cu,
        .ttbr_reset = 0x02400000u,
        .osc3_hz    = PS_OSC3_HZ,
    },
    {
        .name = "idl",
        .title = "Tamagotchi iD L",
        .rom_base   = PS_ROM_BASE,    .rom_size    = PS_ROM_SIZE,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        /* iD L waits for peer traffic before entering a session. It's quirky like that. */
        .ir_ctx_ptr = 0x00084350u,
        .has_sleep_flag = true,
        .sleep_flag = 0x00000FD8u,
        .ttbr_reset = 0x02400000u,
        .osc3_hz    = PS_OSC3_HZ,
    },
    {
        .name = "id",
        .title = "Tamagotchi iD",
        .rom_base   = 0x00C00000u,    .rom_size    = 0x00400000u,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        /* Receive interrupts accept bytes before a session starts. */
        .ir_ctx_ptr = 0x000842BCu,
        .has_sleep_flag = true,
        .sleep_flag = 0x00000EFCu,
        .ttbr_reset = 0x00C00000u,
        .osc3_hz    = PS_OSC3_HZ,
    },
    {
        .name = "id-melody",
        .title = "Tamagotchi iD Melody",
        .rom_base   = 0x00C00000u,    .rom_size    = 0x00400000u,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        /* Receive interrupts accept bytes before a session starts. */
        .ir_ctx_ptr = 0x000842B8u,
        .has_sleep_flag = true,
        .sleep_flag = 0x000011E0u,
        .ttbr_reset = 0x00C00000u,
        .osc3_hz    = PS_OSC3_HZ,
    },
    {
        .name = "4u",
        .title = "Tamagotchi 4U+",
        .rom_base   = PS_ROM_BASE,    .rom_size    = PS_ROM_SIZE,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        .nfc_pn512  = true,
        .bingo_open_pc = 0x02514704u,
        .bingo_play_pc = 0x02510CF0u,
        /* Bingo, then gashapon. */
        .bingo_done_pc = { 0x02510CDAu, 0x02510DB4u },
        .has_sleep_flag = true,
        .sleep_flag = 0x00001440u,
        .wake_press_lost = true,
        .ttbr_reset = 0x02400000u,
        .osc3_hz    = 30000000.0,
    },
    /* Keep "4u-plain": saved settings already use "4u" for the 4U+. */
    {
        .name = "4u-plain",
        .title = "Tamagotchi 4U",
        .rom_base   = PS_ROM_BASE,    .rom_size    = PS_ROM_SIZE,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        .nfc_pn512  = true,
        .bingo_open_pc = 0x024E9C5Au,
        .bingo_play_pc = 0x024E624Cu,
        /* Bingo, then gashapon. */
        .bingo_done_pc = { 0x024E6236u, 0x024E6310u },
        .has_sleep_flag = true,
        .sleep_flag = 0x00001534u,
        .wake_press_lost = true,
        .ttbr_reset = 0x02400000u,
        .osc3_hz    = 30000000.0,
    },
    /* Plus Color uses GPIO IR instead of the UART. */
    {
        .name = "plus-color",
        .title = "Tamagotchi Plus Color",
        .rom_base   = 0x00C00000u,    .rom_size    = 0x00400000u,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        .ir_gpio        = true,
        .ir_gpio_tx_bit = 3,          /* carrier output */
        .ir_gpio_rx_bit = 5,          /* active-low envelope input */
        .ir_code_lo = 0x00C31000u,    /* connect-driver range */
        .ir_code_hi = 0x00C32400u,
        .flash_top_boot   = true,
        .debug_strap_high = true,     /* retail low; high selects debug */
        .has_sleep_flag = true,
        .sleep_flag = 0x00000368u,
        .ttbr_reset = 0x00C00000u,
        .osc3_hz    = 20000000.0,
    },
    /* Hexagontchi reverses the Plus Color debug-strap polarity. */
    {
        .name = "plus-color-hexa",
        .title = "Tamagotchi Plus Color (Hexagontchi)",
        .rom_base   = 0x00C00000u,    .rom_size    = 0x00400000u,
        .a0ram_base = PS_A0RAM_BASE,  .a0ram_size  = PS_A0RAM_SIZE,
        .ivram_base = PS_IVRAM_BASE,  .ivram_size  = PS_IVRAM_SIZE,
        .dstram_base = PS_DSTRAM_BASE, .dstram_size = PS_DSTRAM_SIZE,
        .io_base    = PS_IO_BASE,     .io_end      = PS_IO_END,
        .io_size    = PS_IO_END - PS_IO_BASE,
        .lcd_cmd_addr = PS_LCD_CMD_ADDR, .lcd_data_addr = PS_LCD_DATA_ADDR,
        .ir_base    = PS_IR_BASE,
        .ir_gpio        = true,
        .ir_gpio_tx_bit = 3,
        .ir_gpio_rx_bit = 5,
        .ir_code_lo = 0x00C44300u,    /* connect-driver range */
        .ir_code_hi = 0x00C45700u,
        .flash_top_boot = true,
        /* false means retail P06 is high. */
        .has_sleep_flag = true,
        .sleep_flag = 0x00000000u,
        .ttbr_reset = 0x00C00000u,
        .osc3_hz    = 20000000.0,
    },
};
#define N_DEVICES (sizeof devices / sizeof devices[0])

const DeviceProfile *device_default(void)
{
    return &devices[0];
}

const DeviceProfile *device_at(size_t i)
{
    return i < N_DEVICES ? &devices[i] : NULL;
}

const DeviceProfile *device_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < N_DEVICES; i++)
        if (!strcmp(devices[i].name, name)) return &devices[i];
    return NULL;
}

void device_list(FILE *f)
{
    fprintf(f, "known devices:\n");
    for (size_t i = 0; i < N_DEVICES; i++)
        fprintf(f, "  %-12s %s\n", devices[i].name, devices[i].title);
}

/* Reject profiles that exceed Emu's fixed backing arrays. */
int device_check(const DeviceProfile *d, FILE *f)
{
    int ok = 1;
    struct { const char *what; uint32_t want, have; } fits[] = {
        {"A0RAM",  d->a0ram_size,          A0RAM_MAX},
        {"IVRAM",  d->ivram_size,          IVRAM_MAX},
        {"DSTRAM", d->dstram_size,         DSTRAM_MAX},
        {"IO",     d->io_end - d->io_base, IORAM_MAX},
    };
    for (size_t i = 0; i < sizeof fits / sizeof fits[0]; i++)
        if (fits[i].want > fits[i].have) {
            fprintf(f, "[device] %s: %s needs %u bytes but the array holds %u - "
                       "raise the ceiling in emu.h\n",
                    d->name, fits[i].what, fits[i].want, fits[i].have);
            ok = 0;
        }
    /* Wide-access subtraction checks require regions of at least four bytes. */
    struct { const char *what; uint32_t size; } wide[] = {
        {"ROM",   d->rom_size},
        {"A0RAM", d->a0ram_size},
        {"IO",    d->io_end - d->io_base},
    };
    if (d->io_end < d->io_base) {
        fprintf(f, "[device] %s: io_end is below io_base\n", d->name);
        return 0;
    }
    /* io_size must match the cached I/O span used by the decoder. */
    if (d->io_size != d->io_end - d->io_base) {
        fprintf(f, "[device] %s: io_size %u disagrees with io_end - io_base %u\n",
                d->name, d->io_size, d->io_end - d->io_base);
        ok = 0;
    }
    for (size_t i = 0; i < sizeof wide / sizeof wide[0]; i++)
        if (wide[i].size < 4) {
            fprintf(f, "[device] %s: %s region is %u bytes; the decoder needs at "
                       "least 4 for 32-bit accesses\n",
                    d->name, wide[i].what, wide[i].size);
            ok = 0;
        }
    /* Address zero is valid, but a nonzero address must enable the field. */
    if (d->sleep_flag && !d->has_sleep_flag) {
        fprintf(f, "[device] %s: sleep_flag %08x set but has_sleep_flag is not - "
                   "set both or neither\n", d->name, d->sleep_flag);
        ok = 0;
    }
    /* GPIO IR needs two distinct P0 bits. */
    if (d->ir_gpio &&
        (d->ir_gpio_tx_bit > 7 || d->ir_gpio_rx_bit > 7 ||
         d->ir_gpio_tx_bit == d->ir_gpio_rx_bit)) {
        fprintf(f, "[device] %s: ir_gpio bits tx=%u rx=%u - need two distinct "
                   "bits 0-7\n", d->name, d->ir_gpio_tx_bit, d->ir_gpio_rx_bit);
        ok = 0;
    }
    /* The optional connect-driver range must be ordered and inside ROM. */
    if ((d->ir_code_lo != 0) != (d->ir_code_hi != 0) ||
        (d->ir_code_lo &&
         (d->ir_code_lo >= d->ir_code_hi ||
          d->ir_code_lo - d->rom_base >= d->rom_size ||
          d->ir_code_hi - d->rom_base > d->rom_size))) {
        fprintf(f, "[device] %s: ir_code range %08x..%08x - need an ordered "
                   "range inside ROM, or neither end set\n",
                d->name, d->ir_code_lo, d->ir_code_hi);
        ok = 0;
    }
    if (d->has_sleep_flag && d->sleep_flag - d->a0ram_base >= d->a0ram_size) {
        fprintf(f, "[device] %s: sleep_flag %08x lies outside A0RAM - the "
                   "stay-awake write would land off the array\n",
                d->name, d->sleep_flag);
        ok = 0;
    }
    return ok;
}
