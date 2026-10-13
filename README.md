# Jetpac for the Sega Master System

A complete port of Ultimate Play the Game's ZX Spectrum *Jetpac* (1983) to the
Sega Master System.  The game logic is re-implemented from the annotated
disassembly of the original, the graphics come from the Spec256 recoloured
artwork, and the whole thing is built with the devkitSMS tool chain
(SDCC + SMSlib) the way the `sega-master-system-skill-for-claude` skill
prescribes, and verified with that skill's `smstest` harness.

* ROM: **`jetpac.sms`** (64 KB, SEGA + SDSC headers, checksum written by `ihx2sms`)
* Symbols for the test harness / debuggers: `jetpac.noi`
* Source package: `jetpac-sms-src.zip` (this directory without the scratch screenshots)

Sources used:

* Spec256 artwork and snapshot: https://github.com/mvvproject/Spec256-Games/tree/master/Jetpac
* Annotated disassembly (`jetpac.skool`): https://github.com/mrcook/jetpac-disassembly
* devkitSMS skill (tool chain, test harness, conventions): https://github.com/haroldo-ok/sega-master-system-skill-for-claude
* devkitSMS / SMSlib / PSGlib by sverx: https://github.com/sverx/devkitSMS

---

## 1. What is in the port

Everything the Spectrum game does, re-implemented rather than emulated, with
no placeholders:

| Area | Status |
|---|---|
| Game selection menu | 1 / 2 players, one / two control pads, start game; same layout and text as the original |
| 1 and 2 player games | alternating turns, separate scores, levels and rocket states per player, `GAME OVER PLAYER n` messages, high score, `1UP` / `2UP` flashing labels |
| Levels / aliens | all 8 alien types in the original order (meteors, squidgies, spheres, jet fighters, UFOs, crossed ships, spacecraft, frogs) with the original movement, homing, spawning, collision and scoring rules; the level sequence wraps as on the Spectrum |
| Rocket | module placement on the platforms, pick-up, carry, drop onto the pad, assembly order, fuel pods, fuel gauge, full-rocket flash, take-off with flames, flight to the next planet, landing, four rocket designs |
| Items | collectible treasures with the original colour/flash types and 250 points |
| Jetman | walk / fly animation in both directions, thrust physics, platform collision, screen wrap, death explosion |
| Laser | random beam length, colour cycling, flicker, the three "eraser" segments, stops at platforms and aliens, max 4 beams, new beam every 4th frame |
| HUD | scores, HI score, both players' lives counters with the lives icon |
| Sound | all beeper effects (laser, pick-ups, rocket building, explosions, thrusters during take-off / landing, start blip) converted to PSG tone bursts driven from the line interrupt |
| System | pause button (NMI), PAL / NTSC detection; the game logic runs at the Spectrum's 50 Hz on both (a 50-on-60 scheduler on NTSC machines) |

Jetman's own jetpack is silent, exactly as on the Spectrum (the `SfxThrusters`
routine of the original is only used for the rocket and the jet-fighter kill).

### Hardware sprites

Hardware sprites (8x16 mode) are used for everything that moves: Jetman,
aliens, rocket modules, fuel pods, items, explosions and the rocket itself.
The sprite attribute table order is rotated every frame so the VDP's
8-sprites-per-scanline limit shows up as flicker instead of missing objects.

The laser beam is the only thing drawn into the background: a 1-pixel line
that can span the whole screen cannot be made of sprites (8 per scanline), so
beam cells are composited into a pool of dynamically allocated background
tiles on top of the nebula.  The same composite mechanism draws the HUD text.

---

## 2. Controls

| Action | Pad |
|---|---|
| Menu: move cursor / select | up, down / button 1 or 2 |
| Walk / fly left, right | left, right |
| Thrust | up or button 2 |
| Fire laser | button 1 |
| Pause | console PAUSE button |

With `TWO CONTROL PADS` selected, player 2 uses pad B.

---

## 3. Building

Requirements

* SDCC 4.2 or newer (4.5.0 was used; `apt-get install sdcc` on Debian/Ubuntu)
* devkitSMS installed in `~/.devkitsms` (`bin/ihx2sms`, `bin/folder2c`,
  `lib/crt0_sms.rel`, `lib/SMSlib.lib`, `lib/PSGlib.lib`, `include/`).
  The skill's `scripts/setup_toolchain.sh` installs exactly this layout and also
  builds the `smstest` harness into `~/.devkitsms/bin`.
* GNU make

```
export PATH="$HOME/.devkitsms/bin:$PATH"
make            # -> jetpac.sms, jetpac.noi (build/ holds the .rel/.asm/.lst/.map files)
make playtest   # boot -> menu -> play -> game over -> menu + VRAM pacing check
make test       # all scenario tests
make clean
```

`DEVKIT=/other/path make` selects another devkitSMS prefix.  The ROM is
built with `--opt-code-speed`; it currently uses 35,956 of 65,536 bytes
(code and tables 14,996 bytes in banks 0/1, graphics in banks 2 and 3).

### Regenerating the graphics and sound tables

The generated files (`data/*.bin`, `src/gen_assets.h`, `src/gen_tables.c`,
`src/gen_sfx.c`) are part of the source tree, so a normal build does not need
the Spectrum material.  To regenerate them:

```
pip install numpy pillow          # asset pipeline
python3 tools/build_assets.py     # graphics, palettes, lookup tables
python3 tools/gen_sfx.py          # PSG sound tables from the beeper routines
```

`tools/build_assets.py` reads the Spec256 files from `../jetpac-src`
(override with `JETPAC_SRC=/path`): `JETPAC.SNA`, `JETPAC.GFX`, `ROM0.GFX`,
`spec256.raw.pal` from the Spec256-Games repository plus the 48K Spectrum ROM
`48.rom` (the recoloured ROM font is addressed through it).  The number of
nebula background tiles can be tuned with `K_NEBULA=<n>` (default 128, which
leaves 95 composite tiles for the HUD and the laser beams).

`tools/skool_blocks.json` (every data block of the disassembly with its
address) is produced by `tools/extract_skool.py` from `jetpac.skool`
(`JETPAC_SKOOL=/path/jetpac.skool`, default `../jetpac-disassembly/jetpac.skool`).

---

## 4. Tests (smstest harness)

`tests/*.txt` are scripts for the skill's `smstest` emulator harness
(`smstest jetpac.sms tests/<name>.txt`; a non-zero exit code means failure).
All of them are deterministic and poke only symbolic addresses (`sym+N`).

| Script | Covers |
|---|---|
| `smoke` | boots to the menu with the display on and the expected colours |
| `playtest` | the definition-of-done run used by `make playtest`: menu → game → flight + laser → explosion → game over → menu, `expectpace 0` |
| `play1`, `play2`, `play3` | starting a game from the menu, flying, firing, aliens, score |
| `play4` | fuelled rocket, take-off, flight to level 2, landing, death by alien |
| `play5` | last life lost → explosion → `GAME OVER` → menu, high score update, PSG log (`shots/p5_psg.log`) |
| `play6` | 2 player game: menu selection, turn switch, game over for each player, return to the menu |
| `play7` | rocket module pick-up / assembly, fuel pod, fuel gauge, collectible item and the scores for each |
| `busy` | stress scene (many beams, aliens, explosions): performance counters, `expectpace 0` (no VRAM data writes closer than 27 cycles during active display) |

`make test` runs all of them (the scripts write their screenshots and logs
to `shots/`, which the Makefile creates).

`tests/dump*.txt`, `prof*.txt`, `fr.txt`, `t1..t3.txt` are the state-dump and
profiling scripts used during development (`tools/prof.py`, `tools/hot.py`).

---

## 5. Source layout

```
Makefile              devkitSMS build (banks 2/3 via folder2c, ihx2sms)
src/main.c            entry point, IM 2 switch, PAL/NTSC detection, frame scheduler, input, pause
src/game.c  game.h    game logic: object slots, Jetman, aliens, rocket, items, lasers, scoring, players
src/render.c          VRAM manager: composite tile pool, laser beams, HUD, sprite builders, menu / game-over screens
src/sound.c           PSG playback of the converted beeper effects (line-interrupt driven)
src/isr.s             IM 2 interrupt handler (frame + line interrupts, pad sampling)
src/gen_assets.h      generated: tile indices and sizes
src/gen_tables.c      generated: sprite part tables, palettes, glyph map, platform cells
src/gen_sfx.c         generated: sound effect tables
data/*.bin            generated graphics (bank 2: nebula, font, platforms, core sprites; bank 3: alien + rocket sets)
tests/                smstest scripts (see above)
tools/build_assets.py asset pipeline (uses jetlib.py, images.py, palette.py, nebula.py, paths.py)
tools/gen_sfx.py      beeper routine -> PSG table converter
tools/extract_skool.py, locate_sna.py, render_sheet.py, palcheck.py
                      disassembly / Spec256 inspection helpers
tools/zxrun.py, zxpass.py, zxslots.py, hot.py
                      run the original snapshot in skoolkit's simulator (reference measurements; need `pip install skoolkit`)
tools/prof.py         build + profile the port with smstest's sampling profiler
shots/                screenshots and logs produced by the test scripts (not in the zip)
screenshots/          selected screenshots (menu, play, laser, 2 player flow, rocket assembly, all alien types, ...)
```

### Technical notes

* **Game loop.** `game_tick` mirrors the original main loop: a per-frame
  mini-pass (Jetman + lasers) and a full object pass of the 11 object slots
  (rocket, 2 items, puff, 6 aliens, Jetman explosion) whose rate is governed by
  the same cost budget the Spectrum's timing implies, so alien speeds, wave
  timing and laser behaviour match the original.
* **Interrupts.** IM 2 with a vector table at `0x7C00`; the frame interrupt
  sets the VBlank flag and samples the pads, a line interrupt every 4 scanlines
  advances the sound sequencer.  All VRAM writes happen in `render_vblank`; the
  few that may spill into active display are hand paced (≥ 28 cycles apart).
* **VRAM map.** tiles 0–159 core sprites (Jetman, items, explosions, flames),
  160–175 current alien set, 176–211 current rocket set, 212–339 nebula,
  340–352 platforms, 353–447 composite pool (HUD text and laser beam cells).
* **RAM.** 4.4 KB of variables (`0xC000–0xD14A`); the stack stays above
  `0xDFB0` in the stress test.
* **Banks.** Code and tables in slots 0/1; bank 2 = nebula, font, platforms,
  core sprites; bank 3 = alien and rocket sets (mapped only while loading).

---

## 6. Known deviations from the Spectrum original

* The random number generator is a 16-bit xorshift instead of the Spectrum's
  R-register based one, so alien waves are not bit-identical to a given
  Spectrum session (they are statistically the same).  Like the original, the
  game timer keeps running on the menu, so each game starts differently.
* The Spectrum's attribute based colour effects are replaced by the Spec256
  artwork; the fuel gauge is shown with recoloured rocket tiles (the
  flashing items are shown / hidden on the original's timer).
* Screen changes (new level, game over, menu) are drawn with the display
  blanked for 10–23 frames instead of being drawn progressively.
* The original's odd 2 player rule is kept: player 1 immediately receives the
  new-rocket bonus life at the start of the game, player 2 does not, so player
  2's first turn shows one life fewer.

---

## 7. Credits and legal

Jetpac © 1983 Ultimate Play the Game / Rare.  This port is a non-commercial
fan conversion made for study and preservation; it contains the original
game's artwork (as recoloured by the Spec256-Games project) and must not be
sold.  Disassembly by Michael R. Cook, Spec256 artwork from the Spec256-Games
project, devkitSMS by sverx, test harness and conventions from the
`sega-master-system-skill-for-claude` project.
