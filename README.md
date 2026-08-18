# tamaemu

A Tamagotchi emulator for Windows. Boots real firmware dumps to a playable
128×128 screen with clickable buttons, wall-clock RTC, sound, and persistent saves.

Supported devices: P's, iD L (all models), iD, iD Melody, 4U+, 4U, Plus Color, and
Plus Color (Hexagontchi).

Connection play is fully fleshed out, so devices can propose, play and exchange gifts!

Cross-model play is spotty (ie, connecting an IDL to a P's) so don't rely on it. It might work but it's not 100% there yet.

## Firmware

This project does not include or download Tamagotchi firmware and never will. You need a
rom dump of a device you own. The file is read locally on your device!

## Controls

|       | A | B | C |
|-------|---|---|---|
| keys  | <kbd>Z</kbd> or <kbd>&larr;</kbd> | <kbd>X</kbd> or <kbd>&darr;</kbd> | <kbd>C</kbd> or <kbd>&rarr;</kbd> |

The on-screen buttons work with clicking, too! These keys are also rebindable in the emulator itself.

- <kbd>+</kbd> / <kbd>&minus;</kbd> — speed the game clock up or down
  (1x to 600x). <kbd>0</kbd> resets to 1x. The Tamagotchi ages faster; animations and
  sound stay at normal speed. Speed resets to 1x on every boot.

## Connection play

Run two instances with different saves; they find each other automatically. Connection play is just like it is on the normal hardware, so if you're confused just look up guides on how to connect them.

| pair | can connect? |
|------|--------------|
| P's + P's | yes |
| iD L + iD L | yes |
| iD + iD | yes |
| iD Melody + iD Melody | yes |
| iD + iD Melody | yes |
| Plus Color + Plus Color | yes |
| Hexagontchi + Hexagontchi | yes |
| Plus Color + Hexagontchi | yes |
| 4U or 4U+ pair | yes |
| mixed pairings | soon... |

## Saves

Saves are stored in `<rom>.sav` next to your ROM file. Delete (or rename) the `.bin` for a
factory-fresh restart.

## Licence

GPL-3.0 — see `LICENSE`.

Tamagotchi firmware is not covered by this licence and is not included; it is
Bandai's, and you supply your own dump! 
