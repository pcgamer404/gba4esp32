# esp-gba — compact session log

GBA emulation on a Freenove FNK0104 (ESP32-S3, 2.8" 320x240 ILI9341, ES8311
audio, FT6336 touch, microSD), based on [44vba](https://github.com/44670/44vba).

## State

| | |
|---|---|
| Ruby | runs, **18.6 fps** |
| Emerald | runs, **11.2 fps** (16MB cart) |
| FireRed | should run (untested), 16MB cart |
| Display | 320x240, game centred 240x160 with border |
| UI | full-screen icon library, 5x3 grid, touch + BOOT button |
| Audio | codec configured, I2S draining, **no sound yet** |
| Touch | works, **calibration not yet run** |
| SD | mounted 4-bit, `/roms`, `/saves`, `/art`, `/packed` |

## Board facts (measured, not from docs)

The vendor sketches are wrong for this board in two places. The schematic's
ESP32-S3 symbol is authoritative:

```
LCD    CS 10  MOSI 11  SCK 12  MISO 13  DC 46  RST -1  BL 45
SD     CMD 40 CLK 38   D0 39   D1 41    D2 48  D3 47      <- NOT 4/5/6/7/2/3
I2S    MCK 4  SCK 5    DO 6    LRC 7    DI 8
I2C    SCL 15 SDA 16          (ES8311 0x18, FT6336 0x38)
amp    AUDIO_EN 1  -> SC8002B SHUTDOWN, 10K pull-up to 3V3
```

GPIO 6 is **I2S_DO**, not SD D0 — hours were lost probing it as an SD line.

## Fixes that were not obvious

1. **`config.h` missing from upstream** — `.gitignore` swallows it; the port
   does not compile as published.
2. **Flash mode must be DIO** — PlatformIO's board manifest forces `qio` into
   the image header, overriding sdkconfig. QIO makes the ROM loader misread the
   bootloader and boot-loop. QIO *does* work for the app once the IDF bootloader
   has enabled the flash QE bit — hence `tools/flash_qio.sh`.
3. **`emuInit()` never called `load_image_preferences()`** — no RTC, no save
   type, so Pokémon hangs in forced blank (`DISPCNT=0x80`) rendering pure black.
4. **SPI transactions cap at 32768 bytes** on the S3; the driver only checks
   `max_transfer_sz`, so a 76800-byte frame silently delivered 68 of 160 rows.
5. **CS must stay LOW across a command and its data** — this panel discards the
   operation otherwise. Upstream raised CS after every transfer.
6. **`setuptools<81`** for PlatformIO's IDF builder (`pkg_resources`).
7. **Console baud needs `ESP_CONSOLE_UART_CUSTOM=y`** or Kconfig silently keeps
   115200.

## Sparse ROM — how 16MB carts fit in 13.94MB

GBA carts are padded to a power of two, and the padding is **interior**:
FireRed is 44.5% `0xFF` with a 5.22MB hole at `0x6C7D38`.

At 64KB page granularity every all-`0xFF` page is dropped on copy and aliased to
**one shared physical page** via `spi_flash_mmap_pages()`. The emulator still
sees a flat contiguous ROM — no paging layer, no runtime SD reads.

| ROM | pages | blank | flash used |
|---|---|---|---|
| FireRed | 256 | 107 | 9.31 MB |
| Emerald | 256 | 63 | 12.12 MB |
| Ruby | 128 | 1 | 7.94 MB |

Second limit found: the MMU data window. 16MB of virtual mapping fails with
`ESP_ERR_NO_MEM`; the code probes down and settles at 244 pages (15.25 MB),
which still covers everything the game reads.

Packed results are cached at `/sd/packed/<name>.pak` + `.map` so a re-pick skips
the scan. It does **not** avoid the flash write, which is most of the ~2 minutes.

## Performance: what worked, what did not

Baseline 13.0 drawn fps -> **18.6** today (Ruby).

| Change | Result |
|---|---|
| Async DMA blit, CS held across frame | **+29%** |
| QIO flash (app only) | +3% |
| 64-byte data cache line | +3% |
| PSRAM 40 -> 80 MHz | +3% |
| SPI 40 -> 60 MHz | 0% |
| `-O3` | **−2%**, reverted |
| Cache burst-wrap | does not link on IDF 4.4 + octal PSRAM |
| `vram` -> internal SRAM | impossible: PSRAM DMA hangs, so `FB` must hold it |
| **Dual-core renderer** | **+11% emulated, drawn fps HALVED** — reverted |

Dual core is the interesting negative: pinning vba-next's `THREADED_RENDERER` to
core 1 worked (after fixing upstream's missing `INIT_RENDERER_CONTEXT`), but the
renderer could not keep pace and the picture updated half as often. The shim is
kept in `thread_esp.c`, disabled, with the numbers recorded.

**60 fps is not reachable by tuning.** The interpreter is 336 KB of `.text`
against ~263 KB of free SRAM, so it cannot live in IRAM. Only a dynamic
recompiler closes a 3-5x gap, and none exists for Xtensa.

## Verifying without eyes on the screen

Register reads on this panel are trustworthy (proved by writing two different
MADCTL values and watching the readback track), which makes the display
self-checking:

- `lcdSelfTest()` writes 8 red pixels through the real blit path and reads them
  back out of frame memory
- `lcdProbeRow()` samples panel RAM while the game runs

That is how the black-screen and truncated-blit bugs were found — the
framebuffer was always correct, only what reached the glass was wrong.

## Open

- **Audio silent.** Codec configures, I2S drains (`43264 frames sent`). Boot
  now plays the same tone at both `AUDIO_EN` polarities to settle whether the
  SC8002B's `SHUTDOWN` is active high or low.
- **Touch calibration** not yet run (three crosshairs at boot, skips after 8s).
- Emulator produces audio at ~30% of real time, so even once the amp works
  expect underruns until frame rate or buffering is addressed.

## Usage

```bash
ESP_GBA_BOARD=FNK0104AB ./tools/flash_qio.sh   # build + flash (NOT pio upload)
python tools/bench.py LABEL 20 30              # reset and measure
python tools/play.py shots "wait 240, shot x"  # UART screenshots (bridge boards)
```

## 2026-08-19 (second session): boot loop fixed, dual-core, painter renderer

State at end of session (Ruby attract, measured):

| | emu fps | drawn fps |
|---|---|---|
| start of day (fs=1, single core) | 18.6 | 9 |
| no frameskip, single core | 13.9 | 13 |
| final build (no fixed frameskip) | **22-27** | 11-13 (20/20 in menus) |

Frameskip is GONE (SetFrameskip(0)); heavy scenes drop whole coherent frames
adaptively only when the core-1 renderer misses the deadline.

### What changed (chronological, all measured on the board)
1. **Boot-from-SD death loop fixed** (three bugs: romGetFlashed read the
   header at the sparse-blank page; copy failures were LCD-only and fell
   through to esp_restart; stale NVS maps trusted). Copy failures now park in
   the menu, NVS is invalidated before writes, bad pak caches self-delete.
2. **SD card wedged** by the old loop's interrupted write (protocol-dead,
   powered rail). Needs physical reseat/replacement. Ruby runs from flash
   without it (flat map path).
3. **The sdspi fallback corrupts the heap on failed mounts** (IDF 4.4 cleanup
   bug) -- removed; SDMMC-only with 3 retries.
4. **Serial input never worked on this board variant** (native USB-JTAG, not
   CH340; UART0 pins go nowhere). Dual backend now: commands + frame dumps
   over usb_serial_jtag. bench_fskip/play/pick all work over /dev/ttyACM0.
5. **CPUReset memset overran pix** (hardcoded 4*160*240 = 153600 into an
   80KB buffer) once pix moved internal -- was zeroing the SPI device struct
   and UART lock. Found via heap poisoning; upstream never saw it because it
   over-allocates pix 4x.
6. **pix IS the framebuffer now**: PIX_BE_565 makes CONVERT_COLOR emit panel
   byte order; the per-frame byteswap+copy and the separate FB are gone.
   Skipped frames no longer re-blit (draw metric now counts real blits).
7. **Saves implemented**: /sd/saves/<rom>.sav, load at boot, checksum-settle
   flush, OS_CMD_SAVE (0x06) for host-forced flush. UNTESTED on hardware
   (needs SD back).
8. **USE_TWEAKS + TILED_RENDERING enabled** -- both existed upstream, both
   off in the CMake port. TILED_RENDERING alone: the build had been using
   vba-next's per-pixel BG fallback renderer all along.
9. **Dual core**: THREADED_RENDERER with the 4 contexts used as a ring, ONE
   worker on core 1, single <0> template instances via an active-context
   pointer (GCC ignores IRAM_ATTR/section attrs on template instantiations),
   present signalled after line 159 renders. Adaptive whole-frame drop keeps
   core 0 from ever spinning a frame it can't afford.
10. **mode0RenderLineFast**: painter's-algorithm line renderer for the
    no-alpha/no-window/no-mosaic case. BGs painted back-to-front by priority
    into a 555 line + priority byte-map, blank 4bpp tile rows skipped, OBJ
    overlaid with correct priority + semi-transparent alpha, one 555->BE565
    conversion at the end. Pixel-verified against Ruby title/menu via USB
    screenshots. ~105us/line vs ~250 for the stock path.
11. gbaover to DROM (72KB of .data freed), io_registers 32KB->1KB (twice:
    global + per-ctx), thumb handlers + CPULoop in IRAM (+2.7%).

### Where the time goes now (BENCH cpu/wait/w_* fields)
- core 0: ~65% interpreter+system, ~30% still waiting on kept frames.
- core 1: painter ~20-30%, sprites ~10%.
- Interpreter alone would run ~34-38 fps. That is the current ceiling.

### Next levers, in order
1. Interpreter per-instruction overhead (thumbExecute dispatch, busPrefetch
   modelling, tick accounting). Needed for the 30fps floor and beyond.
2. Sprite pass (stock, iterates all 128 OAM entries per line) ~10%.
3. Audio: chain proven working (mic FFT, both amp polarities); in-game sound
   waits on emu speed reaching ~100% or a resampler.
4. SD back -> prove saves + Emerald/FireRed sparse pack on hardware.

Tools: bench_fskip.py (scratchpad) sets runtime frameskip over USB;
play.py works over /dev/ttyACM0 now (screenshots + key injection).

### End-of-day numbers (Ruby, no frameskip anywhere)

| change | emu fps | drawn |
|---|---|---|
| morning state (fs=1 hardcoded) | 18.6 | 9 |
| no-skip baseline, single core | 13.9 | 13 |
| + dual core, adaptive frame drop | 20.6 | 10 |
| + TILED_RENDERING + painter renderer | 22-26 | 11-13 |
| + prefetch-model removal (fast ticks) | 23-29 | 11-14 |
| + slim 16-slot ring | 25-30 | 12-15 |
| + sprite Y prescan | **26-31.5** | **12-16** |

Menus/dialogue draw at full rate (~20/20). Verified pixel-correct via USB
screenshots: title, save menu, New Game intro (Birch + Marill sprites, text).

Interpreter now ~70% of core 0 at 28 fps => ~25ms/frame: the remaining wall.
Next: thumbExecute dispatch + memory access fast paths, then sound at
near-full speed. Board still needs an SD card for Emerald/FireRed + saves.

## 2026-08-19 (third stretch): SD fixed, Emerald running, ARM hot-set IRAM

- The "dead" Viper card was salvaged: PC card reader power-cycled it (only
  power loss clears an interrupted-write wedge), fix_sd.sh (scratchpad)
  rebuilt the FAT, installed /roms + /packed, verified checksums.
- **Host-side pre-packing works end to end**: tools/pack_rom.py generates
  byte-identical .pak/.map to the on-device scanner; the console used the
  PC-built Emerald cache directly ("using packed cache", 194 pages written,
  63 aliased) -- no 16MB scan on device.
- **Emerald (16MB cart) runs**: BPEJ header, save system armed
  (/sd/saves/....sav), attract emu ~25-26 draw ~12 after the ARM work below.
- tools/trim_rom.py trims trailing pad (FireRed is mostly INTERIOR blank:
  content 9.1MB, 6.75MB interior holes -- the packer handles those).
- **Per-function ARM profiling** (pointer-hash histogram): the top ~15
  handler FUNCTIONS carry ~85% of ARM execution (slots were flat, functions
  are not): ALU ADD/ADDS/SUB/MOV/BIC/CMP variants + B + LDRSB/LDRSH pre-inc.
  Blanket-IRAMing all 261 ALU handlers (84KB) starves the heap below pix's
  80KB -- boot FATAL. The precise set (20 fns, 7.9KB) fits and gives
  cpiA 182 -> 173, cpiT 120 -> 117.
- BENCH line now prints cpiT/cpiA/arm% permanently. Profiler itself removed
  (its hash cost ~25cy per ARM insn -- do not trust cpi numbers from builds
  that carry it).

## 2026-08-19 night: overclock probed, 278 MHz held

- Audio pinout re-verified against Freenove's official FNK0104 docs: our
  firmware already matches (MCK4/BCK5/DO6/WS7, amp-enable GPIO1 driven last,
  I2C 15/16). Doc's "I2S_DOUT GPIO8" is the codec mic path, unused. The two
  speaker pins are the PH1.25 connector off the amp -- not GPIOs.
- clkprobe.c added (boot probe + candidate sweep + hold mode, serial cmd
  0xA5 0x07). Results in FINDINGS §8: 320-via-div1 is dead silicon myth;
  VCO overdrive gives a real 278 MHz (+16% CPU AND +16% flash/PSRAM/APB).
- 278 hold launched with Emerald(J) mid-session; save flushed to SD first
  (139264 bytes -- save FLUSH path proven on the wire). USB dies during
  hold: LCD overlay is the fps proof, physical reset to come back.
- Next: user reads LCD fps + stability; then AOT ARM->Xtensa static
  recompile groundwork (design doc), retune I2S/SDMMC dividers under
  overdrive if pitch/SD complain.

## 2026-08-19 late night: AOT milestone 1 -> the idle-loop jackpot

- Audio FIXED end to end (four stacked bugs, FINDINGS §9); game audio now
  continuous via audioMatchRate (slow-motion pitch until full speed).
- Save round-trip PROVEN: "SAVE: flushed 139264" + "SAVE: loaded 139264"
  across a reboot, Emerald (Japan). Task "save system works" is done for
  Emerald; FireRed same code path.
- hotpc.c: every-64th-instruction PC histogram (PSRAM table -- 32KB in .bss
  evicts pix, learned the hard way), OS_CMD_HOTPC dump. tools/aot/
  hotpc_report.py disassembles the top blocks via capstone.
- Emerald in-game: ONE 64B thumb block = 46.6% of ALL execution -- the
  vblank busy-wait at 0x080008c6 (ldrh [r2,#0x1c]; test bit0; loop). IWRAM
  m4a mixer 22%, thumb m4a sequencer cluster (0x8006600-0x8006b00) ~19%.
  Top 25 blocks = 95%: the AOT translation set is ~3KB, not 200KB.
- espgba_idle_pc: when armNextPC hits the stored PC, cpuTotalTicks jumps to
  cpuNextEvent (VBA-M idle-skip semantics, exact timing). Set over serial
  (OS_CMD_IDLE + PC), persisted in NVS per game code, loaded at start.
- Measured in-game, stock 240: 27.8 -> 33.5 emu (+21%). Profile: cpu 86->63%,
  wait 10->32% (the threaded renderer is now the wall), arm share 22->37%.
- Next: same histogram+idle treatment for FireRed; AOT-translate the m4a
  mixer (IWRAM ARM) and sequencer cluster; then renderer wall.

## 2026-08-20 early: full speed in light scenes; walls quantified

- Idle table baked into firmware: BPEJ 0x080008c6, BPRJ 0x080008aa (FireRed
  found STATICALLY by searching its ROM for Emerald's exact spin bytes
  918b181c084000 28fad0 -> tools note: search the REAL bytes, hand-assembled
  patterns miss). Ruby AXVJ: no exact match, needs a live histogram.
- NVS override still wins over the table (OS_CMD_IDLE).
- On-glass telemetry: overlay is now "emu/drawn cNN wNN aNN" (cpu%, renderer
  wait%, arm share) -- BENCH survives 278 holds via the LCD.
- Frame limiter (sample-paced: 47872 pairs = 1 emulated second; per-call
  pacing FAILED, one emuRunFrame can emulate >1 GBA frame under drop logic).
  Intro/menus now cap at true 59.7 fps with cpu at 32% -- first real-time
  scenes ever. Rebase-on-lag: heavy scenes keep every fps they can get.
- In-game Emerald overworld at 240+idle: 28-31 emu. The two walls, measured:
  interpreter cpu 64% (arm share 38-43% = m4a mixer -> AOT target #1) and
  renderer wait 30-32% (core-1 PPU -> target #2 or painter extension).
- Flash port note: after USB re-enumeration the device can be ttyACM1;
  flash_qio.sh honors PORT=/dev/ttyACM1.

## 2026-08-20: menu touch fixed; ring widened; 38.6 in-game at stock

- Menu touch hit boxes were displaced 40px up-left: the tap handler
  subtracted GBA_X/Y_OFF but the grid draws at ABSOLUTE panel coords
  (GRID_X0=6, 5*62=310 wide). Subtraction removed; boxes now match icons.
- Renderer ring: slot io_registers[512] -> [64] (renderer reads nothing
  above REG_WINOUT=0x25, grep-verified; timer/P1/IE copies deleted), ring
  16 -> 64 slots (~700B each). In-game 33.5 -> 38.6 emu, wait 30 -> 24%,
  draw 14 -> 19. 96 slots = RAM 51% and pix DMA alloc fails at boot: 64 is
  the ceiling without moving contexts to PSRAM.
- Stock-240 in-game progression today: 26-28 -> 33.5 (idle skip) -> 38.6
  (ring). Intro/menus: true 59.7 capped. With 278 hold: ~45 expected.
- Remaining to 60 in-game: interpreter cpu 71% of wall with arm=38% share
  (the m4a mixer -> HLE/AOT, THE next lever, est. +20-25%); residual
  wait 24%; then 278 (+16%) on top. Ruby idle PC still unfound (needs a
  live histogram; pattern differs from Emerald/FireRed).

## 2026-08-20 cont.: 41 in-game at stock; mixer mapped for translation

- OS_CMD_PEEK (0x0C): hex-dump emulated memory over serial. Used to dump the
  live IWRAM mixer and SoundInfo -- all struct facts verified on-device
  (tools/aot/mixer_notes.md).
- Reverb kill: Emerald runs m4a reverb (SoundInfo->reverb=0x32), an extra
  full-buffer pass per frame. The port zeroes the byte each frame via
  SOUND_INFO_PTR + ident check (game-agnostic). In-game 38.6 -> ~41 emu,
  arm share 38 -> 33%. Music plays dry.
- Mixer entry found: 0x03001b51 thumb (r0=SoundInfo), ARM kernels mapped.
  HLE hook plan written. This is the next big rock (~arm 33% share).
- Stock-240 in-game ladder today: 26-28 -> 33.5 (idle) -> 38.6 (ring) ->
  ~41 (no reverb). Intro/menus capped at true 59.7. 278 stacks +16% on top.
- Flash-port gotcha: ttyACM0/ttyACM1 flips after re-enumeration; pass
  PORT=$(ls /dev/ttyACM* | head -1) to flash_qio.sh.

## 2026-08-20 cont.: auto-278 ON; mixer fully disassembled

- tools/aot/mixer_full.txt: COMPLETE segmented listing of SoundMainRAM
  (thumb/ARM boundaries mapped: entry 0x1b51T, reverb 0x1b5cA, zero-fill
  0x1bb0T, envelope machine 0x1bdeT, kernel A 0x1d40A w/ sample-end 0x1ddc,
  kernel B 0x1e44A = pitch-interp 4-lane ror#8 mixer, wrap-up 0x1ee8T,
  kernel C 0x1f5cA). Kernel B is NOT ABI-clean (branches into caller,
  reads caller stack): hook must be the 0x1b51 entry, full translation.
- auto-278 enabled (NVS): every game start switches to 278 after clean
  driver init. Expected in-game ~47 emu. Serial dies in-game; toggle OFF
  at the MENU over serial (0xA5 0x09 0x00) for dev sessions.
- Next session: write the native mixer in C against mixer_full.txt
  (300-ish insns), NVS kill switch, ear + soak verification. Est. in-game
  ~55 at 278 after it lands; then the ROM sequencer cluster (19%) and
  residual renderer wait close the 60 goal.

## 2026-08-20/21: native mixer LANDED (~50 in-game at 240); features batch

- Native m4a mixer VERIFIED: hits every frame, bails only on compressed
  samples (clean interpreter fallback), mic A/B shows strong music, no
  crashes. In-game Emerald 44 -> ~50 emu at stock 240; arm share 32 -> 0%,
  apu 31 -> 2%. With auto-278: ~57 expected. Remaining wall: renderer
  (wait 30-34%).
- CORRECTION: 0x03002918 is FIRERED's SoundMainRAM (one engine per game,
  different link address), not a second Emerald engine. hleTable:
  BPEJ=0x03001b50, BPRJ=0x03002918. The user's ROMs were renamed
  ("Pokemon Emerald JP.gba" etc) and now include LeafGreen/Sapphire.
- Console moved to USB-Serial-JTAG (sdkconfig.defaults + sdkconfig.esp32s3
  at the REPO ROOT are the real config files; port-esp32s3/sdkconfig is
  stale). GPIO 43/44 freed -> 2x4 button matrix in os.c:
  rows IO2 (dpad) / IO3 (actions), cols IO14/IO21/IO43/IO44.
  docs/button_wiring.svg is the user-facing diagram.
- Debug overlay moved OUT of the game window into a 240x10 border strip
  above it (text + 4-bar battery from GPIO9=ADC1_CH8, x2 divider). Tap the
  top border in-game to toggle. Drawn/blitted once per second.
- Menu: edge taps (left/right 26px) flip library pages.
- tools/scrape_art.py: libretro boxart -> 48x48 BE RGB565 .raw; all five
  carts fetched into ~/roms/art/ -- copy to SD /art when the card visits
  the PC.
- GB/GBC support: NOT started; plan = Peanut-GB core as a second engine,
  menu keyed by extension. Next session candidate.

## 2026-08-21: settings/cog, buttons-nav, touch L/R, serial art push

- Settings screen (gear icon top-right in picker, or SELECT on the matrix):
  Debug overlay / Overclock 278 / Native audio mix toggles, NVS-backed
  ("ui" namespace: dbg, hle; overclock uses clk/auto278). Navigable by
  touch AND matrix (Up/Down/A/B). Picker itself: dpad moves, A/Start play,
  Select = settings, gear tap = settings, edge taps = pages.
- Touch L/R: in-game, holding the LEFT border = L, RIGHT border = R
  (bits 9/8), polled every frame at 400kHz I2C (~0.2ms). Top-border tap
  toggles the debug strip and persists to NVS.
- OS_CMD 0x10 PUTFILE: [1B pathlen][path rel to /sd][4B size LE][data]
  -> writes to SD, echoes "PUTFILE <path> got/size OK|FAIL". Used to push
  the five 48x48 boxart .raw files into /sd/art without moving the card.
- Charging confirmed from schematic: TP4054, ~100mA (R11=10K PROG), charge
  LED, battery on the BAT PH2.0 connector. Works whenever USB is in.
- L/R necessity: not needed for gen-3 Pokemon (help/L=A only); needed for
  other GBA titles -> covered by touch borders.
- NEXT SESSIONS: GB/GBC via Peanut-GB + minigb_apu as components/gb with
  menu-by-extension; GBA renderer wall (~30% wait) for locked 60.

## 2026-08-21 cont.: renderer ring -> PSRAM (192 slots); gnuboy vendored

- threaded_renderer_contexts now heap_caps_calloc'd from PSRAM at first
  ThreadedRendererStart: 192 slots (full frame + margin) vs 64, and
  internal RAM usage fell 44.2% -> 31.8% (40KB freed next to pix).
  Expected: producer wait (~25-30%) largely eliminated -> in-game emu
  should approach the consumer ceiling. UNMEASURED until next flash
  (watcher armed; flashes the newest binary automatically).
- GB/GBC decision: Peanut-GB is DMG-only (12-colour mode just colourizes);
  real GBC needs gnuboy. Vendored retro-go's gnuboy into components/gnuboy
  (cpu/hw/lcd/sound/gnuboy.c, ~1MB with docs); reference glue saved at
  scratchpad rg/retro-core/main/main_gbc.c. NEXT SESSION: write
  port-esp32s3 glue (gnuboy_init/load/run + pix blit 160x144 + matched
  audio + matrix keys + .gb/.gbc menu routing + SRAM saves), THEN enable
  the extensions in sd.c scan.
- Peanut-GB header also vendored under components/gb (unused for now;
  delete if gnuboy covers everything).

## 2026-08-21 final: full package deployed

- Box art: paced PUTFILE (8KB rx ring + 12KB/s host pacing) -> 5/5 OK on
  /sd/art. Menu shows real covers from the next picker visit.
- Final build flashed: gnuboy GB/GBC core (routes by extension, SRAM
  autosave to /sd/saves/*.srm, untested until a .gb/.gbc lands in /roms),
  PSRAM 192-slot renderer ring, native m4a mixer (BPEJ+BPRJ), idle-skip,
  auto-278, settings menu (Select), button-only controls:
  Select+Left/Right = L/R, Select+Up = debug strip toggle. In-game touch
  removed per user request; menu touch retained as a bonus.
- AWAITING USER: in-game fps readout from the border strip (expected
  high-50s at 278 with the ring), box art confirmation, button wiring
  (diagram: docs/button_wiring.svg).

## 2026-08-21/22: white-screen crash loop fixed; GB/GBC LIVE at 60fps

- ROOT CAUSE of "all GBA roms white screen": auto-278 + the PSRAM renderer
  ring = INT_WDT crash loop on every game start (PSRAM at 92.8MHz, +16%
  over its 80MHz timing calibration, hammered from both cores). FIX:
  overclock capped at x13 = 260MHz (PSRAM 86.7, +8%) -- soak-verified
  75s in FireRed, stable. x14 remains in the sweep data but is excluded
  from hold selection while the ring lives in PSRAM.
- GB/GBC blocker: the sparse-mmap probe loop floored at 64 pages, so any
  cart under 4MB never mapped (ESP_ERR_NO_MEM). Floor now 1 page.
  Pokemon Red JP: cart detected (MBC1 512K), GBBENCH emu=60.0 -- FULL
  SPEED with audio at stock 240 (GB path runs without overclock; serial
  stays alive in GB games).
- User added 7 GB/GBC carts; box art scraped for ALL 12 games now
  (GB/GBC repos + JP romaji aliases + Yellow's long catalog name).
  5 GBA covers already on card; 7 GB covers queued on an opportunistic
  pusher (fires at the next menu visit).
- Current measured state: GB = locked 60. GBA in-game ~45-50 at 240,
  x1.08 at 260; renderer ring verified (draw 30). Remaining GBA-60 work:
  sequencer AOT translation (0x8006600 cluster) + renderer consumer.

## 2026-08-22: art complete (12/12), autostart hold-off, all systems green

- Serial activity now freezes the menu autostart countdown; the 7 GB/GBC
  covers pushed 7/7 OK in one menu visit. All 12 games have box art.
- DEPLOYED STATE: GBA at 260MHz (mixer HLE + idle skip + 192-slot PSRAM
  ring) ~50 in-game; GB/GBC via gnuboy at locked 60 with audio; button
  matrix firmware ready (docs/button_wiring.svg); settings via Select/gear;
  L/R = Select+Left/Right; debug strip = Select+Up; battery gauge; saves
  proven (GBA) / autosave (GB .srm).
- OPEN: GBA 50 -> 60 (sequencer AOT at 0x8006600 cluster + renderer
  consumer); Ruby idle PC (needs one histogram run); GB path untested for
  GBC-mode titles (Crystal/Gold/Silver) -- likely fine, verify by playing.

## 2026-08-22 cont.: GB scaler race fixed; settings chip moved

- "Duplicated bottom" on scaled GB = DMA race: each band was scaled into
  gbBand while the PREVIOUS band's async blit still read it. Fix: drain
  (lcdWaitFB) before scaling each band, queue after. Verified: Red at
  locked 60 emu with the 240x216 scale, clean picture.
- SETTINGS chip moved to bottom-right (was over the game grid); tap zone
  moved with it, SELECT unchanged.
- Known polish item: GB drawn fps = 15 (SPI budget for the 103KB scaled
  frame); dirty-line tracking would raise it if play-testing wants more.
- NEXT SESSION: GBA 50 -> 60, sequencer AOT (0x8006600 cluster) using the
  proven dump/disassemble/translate pipeline; then Ruby idle PC.

## 2026-08-20: FireRed boot-loop ROOT CAUSE -- the 16MB MMU window, not code

- SYMPTOM: FireRed crash-looped at boot (LoadProhibited in thumb47, EXCVADDR=0,
  deterministic) while Emerald ran fine on the identical firmware. Autostart
  relaunched the crashing game forever, so the console looked bricked.
- ROOT CAUSE: the IDF flash-mmap DATA pool is MMU region 0 only = 256 x 64KB
  = 16MB, shared with the app's own code+rodata mapping (11 pages now that
  gnuboy/menu/painter grew the binary). A 16MB cart can therefore never fully
  map: FireRed got 244/256 pages. FireRed JP keeps REAL data up to page 253
  (15.9MB); Emerald JP is blank above page 243. FireRed read garbage where its
  own data belonged, jumped through a junk pointer, and died. NOT the mode-1
  painter, NOT idle skip, NOT HLE (all bisected live over serial first).
- FIX (main.cpp): after the short driver grant, hand-map the missing tail
  pages with ROM Cache_Dbus_MMU_Set into the free VA directly after the grant
  (spills into MMU region 1, 0x3D000000..0x3D7FFFFF, unused below the PSRAM
  window). Each claimed slot is verified MMU_INVALID first. Virtually
  contiguous -> emulator still sees one flat 16MB ROM.
  Verified: "tail 12 page(s) hand-mapped ... full 16.00 MB visible", FireRed
  boots, BENCH ~55 emu at 240MHz stock.
- GUARDS added: (1) if a short map would drop a REAL (non-blank-alias) page
  and the hand-map can't fix it, refuse to start with an on-screen message
  instead of crash-looping; (2) menu skips autostart when the previous boot
  ended in a panic (esp_reset_reason() == ESP_RST_PANIC) -- one crash can
  never loop the console again.
- DEAD END, do not retry: CONFIG_SPIRAM_FETCH_INSTRUCTIONS/RODATA do NOT free
  mmap pool pages -- they remap the same VA to PSRAM, entries stay busy
  (measured: still 244/256). Reverted.
- BUILD GOTCHA: plain `pio run` builds the GENERIC board (undefined
  lcdFillScreen at link if lucky, wrong pins if not). Always
  `ESP_GBA_BOARD=FNK0104AB pio run` / use tools/flash_qio.sh with that env.
  Also penv needs setuptools<81 after any PlatformIO update (pkg_resources).

## 2026-08-20 night: the SECOND FireRed "not booting" -- damaged SD data, proven byte-by-byte

- SYMPTOM (new board): FireRed crash <1s at boot, LoadProhibited BX-to-~0,
  same signature as the MMU bug -- but the tail hand-map reported success and
  the old board ran the identical firmware fine.
- METHOD: (1) BADJUMP guard in the prefetch macros converts the host panic
  into an emulated-context dump + game reset -- one line gave pc=0xfffffffe,
  lr=0x081c1453 (inside m4a SoundMain), registers full of 0xFFFFFFFF: the
  game was calling function pointers read as ERASED FLASH. (2) PEEK sweep of
  all 256 pages vs the host ROM: 86 pages of real data read as 0xFF
  (63-108, 192-217, 240-253 -- large contiguous runs = storage damage, not a
  classification bug).
- ROOT CAUSE: the SD card's FireRed data was damaged (most plausibly during
  the pre-guard overclocked session; SDMMC clock rides the overdriven PLL).
  The old board coasted on its earlier good flash pack; the new board packed
  from damaged source and faithfully installed the holes.
- REPAIR: PUTFILE cap raised 2MB->32MB; clean ROM pushed over USB (paced
  ~48KB/s -- the receiver reads byte-at-a-time, faster streams overflow its
  8KB buffer), ack-verified 16777216/16777216 OK; pack caches STALE'd; flash
  evicted via a Red pick (name-match "already in flash" otherwise SKIPS the
  repack -- lost an hour to that); true rescan gave the known-good signature
  150 written/107 blank; FireRed runs at 55 fps from both boot paths.
- PERMANENT GUARDS now in the code:
  * prefetch bad-jump guard: dump emulated context, reset the GAME, console
    stays up;
  * after 2 bad-jump resets in one session: invalidate the flashed-pack NVS
    record and reboot -- the next pick re-packs from SD (self-healing);
  * HLE mixer: first-fire code-signature check (disarms on mismatch) and
    validate-before-commit epilogue (a wrong frame can no longer clobber
    registers on the way out);
  * BENCH carries keys= for stuck-button visibility.
- LESSON for future sessions: "FireRed does not boot" has now had THREE
  distinct causes (MMU window, OC flash-write corruption, SD data damage).
  Never pattern-match to the previous fix; the BADJUMP dump + PEEK sweep
  now identify the layer in minutes.

## 2026-08-21: Emerald-US white-title root cause -- the unverified mode-1 painter

- Screenshot bisect (SHOT frames, framebuffer-level): GameFreak logo (mode 0)
  renders perfectly; everything after goes white WITH mode1RenderLineFast
  routed, and the attract loop progresses normally (copyright screen came
  back around) with it disabled. The painter shipped unverified during the
  60fps push and every white-game report since traces to it.
- mode1RenderLineFast/fastPaintAffineBG2 are now UNROUTED (code kept for a
  future verified attempt). Suspect: the BG2X/BG2Y writeback interacting
  with slow-path lines (mosaic bail / effect lines double-advance the
  affine reference), and/or the fade path. Verify with SHOT bisects per
  scene before ever re-routing.
- Separate real issue fixed the same night: the 260MHz overclock overdrives
  the LCD SPI clock past this ILI9341's tolerance -- panel white ~10s into
  gameplay while the game runs on. LCD_SPI_HZ 60 -> 55MHz (59.6 under OC).

## 2026-08-21 cont.: mode-0 painter was ALSO white -- both painters unrouted

- The Emerald intro movie (mode 0, BG0-3+OBJ, DISPCNT=1f40) rendered white
  through mode0RenderLineFast too; FireRed's scenes happened to survive it.
  Screenshot bisect either side of the single routing change: white -> the
  full field scene. Both fast painters are now unrouted; slow templates
  everywhere. Emerald US verified through its entire attract cycle.
- Cost: heavy intro scenes ~22-26 fps stock (+OC on the glass now that the
  LCD runs 55MHz). Correctness ships; painters may return only with
  per-scene SHOT verification.
