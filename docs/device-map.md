# Device profile reference

`DeviceProfile` is the emulator's per-machine hardware map. The structure is
declared in `src/emu.h`; the eight profiles are defined in `src/device.c`.
Unspecified fields are zero or false.

This document records the values the emulator uses. It is also the place to
check a value before changing a profile comment or treating two machines as
interchangeable.

## Reset vectors and clocks

`ttbr_reset` is the vector-table base loaded into TTBR by `cpu_reset()`.
`cpu_reset()` then reads the first 32-bit word at that address into `pc`; that
word is the reset vector. They are related values, but they are not the same
field.

`osc3_hz` is the board's OSC3 crystal frequency. It is defined per profile in
`src/device.c`; the four 18.432 MHz rows use `PS_OSC3_HZ` from `src/emu.h`.
The CMU model in `src/periph.c` derives the running MCLK from `osc3_hz` and the
firmware's SCCR/PLLC settings. Plus Color firmware can divide its 20 MHz OSC3
clock to a 10 MHz MCLK at runtime.

| Device key | Window title | `ttbr_reset` | Reset vector | `osc3_hz` | Clock note |
| --- | --- | ---: | ---: | ---: | --- |
| `ps` | Tamagotchi P's | `0x02400000` | `0x024FDFE6` | 18.432 MHz | OSC3 drives MCLK directly. |
| `idl` | Tamagotchi iD L | `0x02400000` | `0x024CA8FE` | 18.432 MHz | Same CMU setup as P's. |
| `id` | Tamagotchi iD | `0x00C00000` | `0x00C907FA` | 18.432 MHz | Vector table is at the NOR base. |
| `id-melody` | Tamagotchi iD Melody | `0x00C00000` | `0x00C939EA` | 18.432 MHz | Vector table is at the NOR base. |
| `4u` | Tamagotchi 4U+ | `0x02400000` | `0x024F6256` | 30 MHz | OSC3 drives MCLK directly. |
| `4u-plain` | Tamagotchi 4U | `0x02400000` | `0x024C680A` | 30 MHz | `4u` already names the 4U+; this key preserves saved settings. |
| `plus-color` | Tamagotchi Plus Color | `0x00C00000` | `0x00C2F0EC` | 20 MHz | Firmware switches MCLK between 20 and 10 MHz. |
| `plus-color-hexa` | Tamagotchi Plus Color (Hexagontchi) | `0x00C00000` | `0x00C4215E` | 20 MHz | Same divider behavior as Plus Color. |

## Field reference

| Field | Meaning | Used by |
| --- | --- | --- |
| `rom_base`, `rom_size` | CPU address and byte length of the NOR image. | Memory decoder, loader, save persistence. |
| `a0ram_base`, `a0ram_size` | A0RAM address range. | Memory decoder, persistent RAM, sleep and IR state. |
| `io_base`, `io_size`, `io_end` | Internal peripheral window. `io_size` must equal `io_end - io_base`. | Memory decoder and peripheral model. |
| `ivram_base`, `ivram_size` | Internal video RAM address range. | Memory decoder. |
| `dstram_base`, `dstram_size` | DSTRAM address range. | Memory decoder and diagnostic state. |
| `lcd_cmd_addr`, `lcd_data_addr` | LCD command/index and data ports. | LCD peripheral path. |
| `ir_base` | Base of the eight-register IR/PN512 UART block. | IR and NFC peripheral path. |
| `ir_mode_flag` | Absolute A0RAM byte that says the firmware is inside an IR session. It also enables receive-side session gating. | IR receive gate. |
| `ir_ctx_ptr` | DSTRAM pointer to an IR context whose state byte is at `IR_CTX_STATE`. This is diagnostic state, not a receive gate. | IR tracing. |
| `has_sleep_flag` | Says `sleep_flag` is known, including the valid address `0x00000000`. | Stay-awake logic and profile validation. |
| `sleep_flag` | A0RAM state byte held at zero by stay-awake behavior. | Main loop. |
| `wake_press_lost` | Firmware consumes the press that wakes it; accelerated clocks therefore keep sleep disabled. | Main loop. |
| `nfc_pn512` | The `ir_base` UART is connected to a PN512 rather than an IR peer. | PN512 routing, reset, reporting. |
| `bingo_open_pc` | PC where bingo/gashapon opens a store NFC touch. | Automatic NFC touch. |
| `bingo_play_pc` | PC where bingo records a completed play. | Automatic NFC touch release. |
| `bingo_done_pc` (2 entries) | Return PCs for bingo and gashapon touch routines. | Automatic NFC touch release. |
| `ir_gpio` | Firmware uses GPIO carrier/envelope IR instead of the UART block. | GPIO IR model and automatic linking. |
| `ir_gpio_tx_bit`, `ir_gpio_rx_bit` | P0 carrier-output and active-low receiver-envelope bits. | GPIO reads/writes. |
| `ir_code_lo`, `ir_code_hi` | Half-open ROM range of the polled IR driver. | Automatic-link activity detection. |
| `flash_top_boot` | NOR parameter sectors are in the top 64 KB instead of the bottom 64 KB. | Flash sector erase geometry. |
| `debug_strap_high` | Debug mode is selected by P06 high. False means debug is selected by P06 low. | P0 input model. |
| `ttbr_reset` | Vector-table base loaded on CPU reset. | CPU reset. |
| `osc3_hz` | OSC3 crystal frequency in hertz. | CMU, timers, link/NFC timing, audio. |
| `name` | Command-line `--device` key and source/suffix for the per-device DLC-folder setting key (`DlcDir_%s`). | Device selection and launcher. |
| `title` | Human-readable device name. | Window title, logs, device list. |

## DLC device map

The shared DLC installer keeps its own device map in `tools/dlc.c`.
`DlcDevice` describes one device's routing and storage layout; its `DlcKind`
entries describe individual stores. Use the `dlc_device_*` lookup and
validation functions when selecting a device or checking a row.

## Shared memory and peripheral map

All eight profiles use the same RAM, I/O, and LCD addresses. The NOR and TTBR
are the parts that split into 8 MB and 4 MB families.

| Field | Value |
| --- | ---: |
| `a0ram_base` | `0x00000000` |
| `a0ram_size` | `0x00008000` (32 KB mapped) |
| `ivram_base` | `0x00080000` |
| `ivram_size` | `0x00003000` (12 KB) |
| `dstram_base` | `0x00084000` |
| `dstram_size` | `0x00000800` (2 KB) |
| `io_base` | `0x00300000` |
| `io_end` | `0x00302000` (exclusive) |
| `io_size` | `0x00002000` (8 KB) |
| `lcd_cmd_addr` | `0x00600000` |
| `lcd_data_addr` | `0x00600001` |
| `ir_base` | `0x00300B10` |

| Device keys | `rom_base` | `rom_size` |
| --- | ---: | ---: |
| `ps`, `idl`, `4u`, `4u-plain` | `0x02000000` | `0x00800000` (8 MB) |
| `id`, `id-melody`, `plus-color`, `plus-color-hexa` | `0x00C00000` | `0x00400000` (4 MB) |

## IR and NFC fields

| Device key | `ir_mode_flag` | `ir_ctx_ptr` | `nfc_pn512` | GPIO IR | Driver PC range |
| --- | ---: | ---: | --- | --- | --- |
| `ps` | `0x00000DB3` | `0` | no | no | none |
| `idl` | `0` | `0x00084350` | no | no | none |
| `id` | `0` | `0x000842BC` | no | no | none |
| `id-melody` | `0` | `0x000842B8` | no | no | none |
| `4u` | `0` | `0` | yes | no | none |
| `4u-plain` | `0` | `0` | yes | no | none |
| `plus-color` | `0` | `0` | no | P0.3 TX, P0.5 RX | `0x00C31000..0x00C32400` |
| `plus-color-hexa` | `0` | `0` | no | P0.3 TX, P0.5 RX | `0x00C44300..0x00C45700` |

The ranges are half-open: the low address is included and the high address is
excluded. `ir_base` remains populated on GPIO-IR profiles because it is part of
the shared map, but `ir_gpio` selects the GPIO path.

## Sleep, flash, and debug strap

| Device key | `has_sleep_flag` | `sleep_flag` | `wake_press_lost` | `flash_top_boot` | `debug_strap_high` |
| --- | --- | ---: | --- | --- | --- |
| `ps` | yes | `0x0000146C` | no | no | no |
| `idl` | yes | `0x00000FD8` | no | no | no |
| `id` | yes | `0x00000EFC` | no | no | no |
| `id-melody` | yes | `0x000011E0` | no | no | no |
| `4u` | yes | `0x00001440` | yes | no | no |
| `4u-plain` | yes | `0x00001534` | yes | no | no |
| `plus-color` | yes | `0x00000368` | no | yes | yes |
| `plus-color-hexa` | yes | `0x00000000` | no | yes | no |

`plus-color-hexa` is why `has_sleep_flag` exists separately from
`sleep_flag`: address zero is valid. `flash_top_boot` selects the Plus Color
family's top parameter sectors. For `debug_strap_high`, false means a retail
unit presents P06 high and debug mode pulls it low; Plus Color reverses that
polarity.

## Automatic bingo and gashapon touch

Only the 4U family populates these fields.

| Device key | `bingo_open_pc` | `bingo_play_pc` | `bingo_done_pc[0]` | `bingo_done_pc[1]` |
| --- | ---: | ---: | ---: | ---: |
| `4u` | `0x02514704` | `0x02510CF0` | `0x02510CDA` | `0x02510DB4` |
| `4u-plain` | `0x024E9C5A` | `0x024E624C` | `0x024E6236` | `0x024E6310` |

All other profiles leave the four PC values at zero.

## Profile validation

`device_check()` runs at startup and rejects profiles that would make the
memory decoder unsafe or silently disable optional behavior. It checks:

- RAM and I/O ranges fit the fixed arrays in `Emu`;
- ROM, A0RAM, and I/O regions are large enough for wide accesses;
- `io_size` matches `io_end - io_base`;
- a nonzero `sleep_flag` has `has_sleep_flag` set;
- GPIO IR uses two distinct P0 bits in the range 0 through 7;
- both IR driver endpoints are present, ordered, and inside the ROM; and
- a known sleep flag lies inside A0RAM.

When a new field is added to `DeviceProfile`, add it to the field table and to
each per-device table in this document before removing its source comment.
