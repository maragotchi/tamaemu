# tamaemu

A Tamagotchi Color emulator for Windows.

Supported devices: P's, iD L (all models), iD, iD Melody, 4U+, 4U, Plus Color, and
Plus Color (Hexagontchi).

This emulator will not work for deka devices or retail-link station devices. It also will not work for anything not listed above.

Same-model connection play is fully fleshed out, so devices can propose, play and exchange gifts!

Cross-model connection play is spotty (ie, connecting an IDL to a P's) so don't rely on it. It might work but it's not 100% there yet.

## Firmware

This project does not include or download Tamagotchi firmware and never will. You need a
rom dump of a device you own. The file is read locally on your device and NEVER uploaded or stored anywhere.

## Controls

|       | A | B | C |
|-------|---|---|---|
| keys  | <kbd>Z</kbd> or <kbd>&larr;</kbd> | <kbd>X</kbd> or <kbd>&darr;</kbd> | <kbd>C</kbd> or <kbd>&rarr;</kbd> |

The on-screen buttons work with clicking, too! These keys are rebindable in the emulator settings.

- <kbd>+</kbd> / <kbd>&minus;</kbd> — speed the game clock up or down
  (1x to 600x). <kbd>0</kbd> resets to 1x. 
  When you speed the emulator up, your Tamagotchi ages faster. Animations and sound stay at normal speed.

## Connection play

Run two instances with DIFFERENT saves. They find each other automatically. Connection play is just like it is on the normal hardware, so if you're confused just look up guides on how to connect them.

| pair | can connect? |
|------|--------------|
| P's + P's | yes |
| iD L + iD L | yes |
| iD + iD | yes |
| iD Melody + iD Melody | yes |
| iD + iD Melody | yes |
| Plus Color + Plus Color | yes |
| Hexagontchi + Hexagontchi | yes |
| 4U or 4U+ pair | yes |
| mixed pairings | soon... |

## Saves

Saves are stored in a folder where the .bin was selected. They're titled after the device you're currently playing. If the folder doesn't exist yet, the emulator will create one on your behalf. 

## Licence

GPL-3.0 — see `LICENSE`.

Tamagotchi firmware is not covered by this licence and is not included; it is
Bandai's, and you supply your own dump! 
