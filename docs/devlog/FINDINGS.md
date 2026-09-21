# Findings â€” dynarec investigation and performance

Companion to `SESSION.md`.

**Historical engineering log:** many measurements in this file were made on
the earlier Freenove FNK0104AB hardware. They are retained as development
history and should not be treated as the current handheld hardware reference.

For the current ESP32-S3 handheld pinout and hardware configuration, see
`WIRING.md` and `components/esp_gba/config.h`.
---

## 0. Corrections from the 2026-08-19 session (measured, not assumed)

- **"Audio silent" was WRONG.** A microphone-FFT test (USB webcam mic +
  `amp_record.py`/`amp_analyze.py`) shows the speaker plays the boot tone at
  ~5x noise floor at BOTH `AUDIO_EN` polarities. The amp is effectively always
  on; the polarity question was moot. What sounded like silence in-game is the
  rate mismatch: the emulator produces samples at ~30% of real time, so I2S
  underruns ~70% of the time. Audio quality IS the frame-rate problem.
- **The boot test tone was never 440 Hz.** `audioTestTone()` looped one sine
  cycle over 256 samples at 48kHz = 187.5 Hz, mislabelled. Fixed to honour its
  `hz` argument.
- **"Re-picking the same game boots instantly" had been broken** since sparse
  packing landed: `romGetFlashed()` verified the header at partition offset
  0xAC, which sparse packing reserves for the shared BLANK page. It always saw
  0xFF, treated flash as empty, and re-flashed the same cart on every boot.
  The 'AXVJ' garbage message previously blamed on the gpsp-reload detour was
  (also) this bug.
- **The Emerald `.pak` cache on the SD was truncated** (fread -> EIO at slot 1)
  and every copy-failure path was LCD-only, which turned an I/O error into an
  invisible, unattended erase->fail->reboot loop that eventually wedged the SD
  card itself (protocol-dead until physically power-cycled; the board never
  cuts SD VDD). Failures now log to serial, a failed copy parks in the menu
  with autostart disabled, NVS is invalidated before each write, and a bad pak
  deletes itself.
- **The USB bridge claim in platformio.ini is stale**: the board enumerates as
  303a:1001 (native USB-Serial/JTAG), not a CH340/CH343.

---

## 1. The gpSP Xtensa dynarec is a dead end (settled, don't revisit)

Repo: `44670/gpsp-reload`, cloned at `/home/charles/Programmation/C/gpsp-reload/`.

**Measured on our board, PokÃ©mon Ruby, same harness:**

| backend | emulated fps | % full speed |
|---|---:|---:|
| gpSP interpreter | 23.7 | 40% |
| gpSP Xtensa dynarec | 5.6 | 9% |

**dhrystone:** interpreter 50.0 fps, dynarec 9.1 fps.

The dynarec is **4â€“5Ã— slower than the interpreter**, consistently, on both a
real 8 MB game and a synthetic benchmark.

### Why â€” from their own `LESSONS.md`

> "Current ESP32-S3 JIT is a correctness scaffold: emitted Xtensa blocks enter
> backend-local ARM/Thumb helpers, not full native ARM-to-Xtensa lowering yet."

> "The 16-instruction Xtensa block cap is a workaround, not a final design."

It is **not a real dynarec**. It emits Xtensa blocks that *call interpreter
helper functions*, capped at 16 instructions per block. So it runs the
interpreter *plus* JIT dispatch overhead. The slowdown is structural, not a
tuning problem.

> An earlier guess in this session blamed PSRAM JIT-cache thrashing. That was
> wrong. The helper-scaffold explanation is the correct one and is documented
> upstream.

### It is correct, just slow

Worth recording so nobody re-tests it:

- Host codegen tests **PASS**; emitted stream starts `36 41 00` = `ENTRY a1, 32`
- QEMU ARM ROM: `result=PASS`, `interp_blocks=0 generic_fallbacks=0 unsupported=0`
- QEMU Thumb ROM: **PASS**, `thumb_blocks=10014576` (PokÃ©mon is Thumb-heavy)

Correctness was never the problem.

---

## 2. `esp32s31/` is a DIFFERENT CHIP â€” its 62 FPS does not apply to us

`gpsp-reload/esp32s31/sdkconfig.defaults` line 2:

```
CONFIG_IDF_TARGET="esp32s31"
```

Not ESP32-S3. It is a RISC-V part: **RV32IM native dynarec**, 320 MHz CPU,
250 MHz PSRAM, `fence.i`, an **RWX PSRAM aperture**, and an 800Ã—480 **RGB**
panel. Stock ESP-IDF cannot even build it.

Consequences:

- Their **62.5â€“66.1 FPS** result is on that board. Unreachable here.
- Their `korvo1_*` drivers (RGB panel, different SDMMC pins, USB XInput) do
  **not** port to our SPI ILI9341 board.
- The RWX PSRAM aperture is *why* their JIT works. The ESP32-S3 has no
  equivalent, which is likely why they moved off Xtensa entirely.

**Do not** try to build `esp32s31/`.

---

## 3. The genuinely valuable thing in that repo: their placement table

From `esp32s31/README.md`. Measured by repeated hard-reset A/B runs. Times are
**per emulated frame**; negative = faster. Their frame budget is ~16 ms, ours is
~42 ms, so percentages here are smaller for us â€” but the *ranking* should hold.

| Placement | Cost | Effect | Their default |
|---|---:|---:|---|
| **ARM interpreter loop (`execute_arm`) in SRAM** | 63,172 B | **âˆ’1.4 to âˆ’1.9 ms** | SRAM |
| Shared render buffer in SRAM | 77,280 B | âˆ’0.50 to âˆ’0.55 ms | SRAM |
| Common RGB565 tile-renderer paths in SRAM | 5,376 B | âˆ’0.10 to âˆ’0.13 ms | SRAM |
| LCD bounce strips 2â†’8 rows | +57,600 B | âˆ’0.27 ms | SRAM |
| GBA IWRAM in SRAM | 32,768 B | âˆ’0.03 to âˆ’0.08 ms | SRAM |
| Sound ring PSRAMâ†’SRAM | 8,192 B | âˆ’0.02 to âˆ’0.03 ms | SRAM |
| GBA VRAM in SRAM | 98,304 B | âˆ’0.10 ms | **PSRAM** (not worth it) |
| Interpreter read map in SRAM | 32,768 B | no gain | PSRAM |
| **One promoted ROM instruction page in SRAM** | 32,768 B | **+0.16 to +0.30 ms REGRESSION** | rejected |
| Profile-selected small helpers | 4,560 B | +0.35 ms REGRESSION | PSRAM |

Two counter-intuitive results worth keeping:

- **Promoting a hot ROM page to SRAM makes things WORSE.** Keeping ROM in the
  PSRAM paging window is faster â€” its separate memory path avoids adding
  contention to the SRAM already shared by CPU, renderer, and LCD DMA.
- **LTO measured NEGATIVE** (`retro_run` 15.294 â†’ 15.426 ms, +0.87%) and is off
  by default in their release build. Matches our own `-O3` result (âˆ’2%).

---

## 4. CORRECTION to `SESSION.md`: "60 fps is not reachable by tuning"

`SESSION.md` currently says:

> "The interpreter is 336 KB of `.text` against ~263 KB of free SRAM, so it
> cannot live in IRAM. Only a dynamic recompiler closes a 3-5x gap."

**The reasoning is wrong.** It treats IRAM placement as all-or-nothing. You do
not need the whole 336 KB interpreter in SRAM â€” you need the **hot loop**.
Their table shows `execute_arm` alone is **63 KB** and worth **âˆ’1.4 to âˆ’1.9
ms/frame**, the single largest win they measured.

We have never tried selective `IRAM_ATTR` placement of the hot interpreter loop
and the hot RGB565 renderer paths. This applies to **our current vba-next build**
regardless of any core swap, and is the highest value/effort item outstanding.

Rough scale on our board: at 23.7 fps the frame is 42.2 ms, so âˆ’1.9 ms is â‰ˆ +5%.
Combined with the render-buffer and tile-renderer placements, plausibly +10â€“15%.
Real, but not a path to 60 fps on its own.

---

## 5. The gpSP-vs-vba-next comparison is NOT yet established

**Do not act on the "gpSP interpreter is 27% faster" claim. It is unverified.**

- gpSP's 23.7 fps was measured **headless** (`GPSP_CORES3SE_LCD=0`) â€” no display
  cost at all.
- Our 18.6 fps for Ruby in `SESSION.md` is **drawn** fps, **with** the SPI blit.

Those are different metrics on different workloads. Our async DMA blit was worth
+29% when introduced, so the display costs real time â€” the true core-vs-core gap
could be much smaller, zero, or negative.

Our firmware already logs **both** numbers:

```
BENCH t=<s>s emu=<emulated fps> draw=<drawn fps> DISPCNT=<x>
```

**The missing measurement is our `emu` figure for Ruby.** If it is â‰ˆ23, gpSP has
no advantage and the whole port is pointless. Get that number before any port
work.

---

## 6. Historical board / workflow facts

### Serial ROM pick (new, committed to our tree)

The menu was touch/BOOT only, so no host script could choose a game â€” which made
reproducible benchmarking impossible. Added:

- `OS_CMD_PICK 0x05` + 1 byte ROM index (`os.h`)
- `serialPick` / `osTakeSerialPick()` (`os.c`)
- pick handling in the menu loop (`menu.cpp`)
- **`osSerialInit()` moved before the menu** in `main.cpp` â€” it used to run after,
  so `uart_read_bytes()` would have failed in the menu
- `tools/bench_pick.py` â€” reset, pick by index, wait for boot, report emu+draw

Send: `A5 05 <index>`, repeatedly, on a timer. Boot chatter means the port is
rarely idle, so do **not** gate sending on "no data received".

### ROM flash writes are the main time sink

- `/sd/packed/<name>.pak` + `.map` cache works and **is** used
  (`MENU: using packed cache ...`), but it only skips the *scan* â€” the flash
  **write** still happens, and that is most of the 2â€“4 minutes.
- NVS records which game is in flash. Re-picking the **same** game skips
  everything and boots instantly. So each game costs one write, ever.
- The gpsp-reload detour clobbered our `rom` partition (its `gamepak` lives at a
  different offset), which is why every game needed repacking afterwards. Boot
  log showed `flash holds '<garbage>', NVS says 'AXVJ' -- treating as empty`.
- The flash write **cannot** be eliminated: the emulator addresses ROM as flat
  memory, so it must be mmap'd flash or PSRAM, and 8 MB does not fit in PSRAM
  alongside the emulator's buffers.

### Serial gotchas

- `pyserial` is **not** in system python. Use `~/.platformio/penv/bin/python`.
- Opening the port toggles DTR/RTS and **resets the board** â€” this aborts an
  in-progress ROM flash write. Do not reconnect while one is running.
- Menu auto-starts after `MENU_AUTOSTART_MS` (12 s) on the first ROM that *fits*,
  which is **Emerald**, not Ruby. Always send an explicit pick.

### Historical board state (2026-08-19)

Restored to our esp-gba firmware and working: LCD up, SD mounted (`Viper
28856MB, 4-bit`), 3 ROMs listed, ES8311 + FT6336 both present on I2C
(`0x18 0x38`). Emerald was mid flash-write when work stopped â€” it may need to
finish or redo that write on next boot.

---

## 7. Historical open items

The following items were open during the earlier hardware investigation. They
are retained as development history and are not current hardware requirements.

- The old Freenove hardware's physical audio path and touch controller had
  unresolved work at this stage of the investigation.
- **Our `emu` fps for Ruby** Ã¢â‚¬â€ see Ã‚Â§5. Blocks any core-swap decision from that
  historical comparison.
- **Selective `IRAM_ATTR` on the hot interpreter loop** Ã¢â‚¬â€ see Ã‚Â§4. This was an
  optimization candidate investigated during the earlier performance work.

## 7b. 2026-08-19 afternoon: the renderer, not the interpreter, is the wall

All measured on Ruby attract mode, cycle counters in CPULoop (BENCH cpu/gfx/
apu/w_* fields). Corrects Â§4/Â§8's assumption that the interpreter dominates:

- No frameskip, single core: **14 fps**, split ~35% interpreter / ~63% PPU.
- The PPU line renderer costs ~72k cycles per line (~310us): tile passes 42%
  of a core, merge 13%, sprites 11%. The interpreter alone would do ~39 fps.
- The tile pass is NOT memory-bound: pointing tile reads at internal SRAM
  (garbage-picture falsification build) changed nothing; -O2 confirmed; the
  inner loop disassembles clean with zero calls. It is plain compute volume,
  ~10x gpsp-reload's renderer for the same content.
- vram->internal is impossible anyway: 133KB contiguous internal DMA pool,
  pix takes 80KB.
- THREADED_RENDERER as shipped (per-line handoff + spin) buys nothing: the
  producer waits 60% of its life on a slower consumer. Ring slots don't help
  (sustained rate mismatch, not variance). **Adaptive whole-frame drop**
  (producer never commits to a frame it would stall on; kept frames stay
  coherent) raised emu 13.6 -> 20.6 with drawn 10.
- Template-instance bloat: the renderer templates were instantiated per ring
  slot (4x code); now all slots render through the <0> instances via an
  active-context pointer. IRAM_ATTR on template functions is silently
  IGNORED by GCC -- template instances stay in flash (.text._Z...).
- USE_TWEAKS (idle-loop burn, fast waitstates, fast DMA): ~+3%. IRAM thumb
  handlers + CPULoop: ~+2.7%. gbaover moved to DROM (was 72KB of .data!).
  ctx/io_registers were 32KB each for a 1KB register file -- shrunk.
- Serial input NEVER worked on this board variant: it reads UART0 (pins
  43/44, unconnected); the host talks over native USB-Serial-JTAG. Fixed
  with a dual backend (uart + usb_serial_jtag driver). frameskip/pick/keys
  over USB now work.

**Path to 30+ fps without frameskip:** a lean mode-0 painter's-algorithm
fast path (no alpha/no window case) targeting <=150us/line. At that point
every frame renders inside the frame budget, drops stop, and both emu and
drawn land at the interpreter's ~35 fps. 60 fps additionally needs ~2x on
the interpreter (vba-next thumbExecute per-insn overhead) -- open.

## 8. Honest status on 60 fps

Not close, and no longer plausibly reachable on this chip:

- The only real dynarec in reach targets a **different SoC**.
- The Xtensa dynarec is a helper scaffold and is *slower* than interpreting.
- Placement tuning is worth maybe +10â€“15%.
- Frameskip raises *drawn* smoothness but not emulation speed (gpSP's own play
  mode ships with fixed frameskip 1 â€” every other frame skipped).

Realistic ceiling with everything above applied: **high 20s fps**, from ~18.6
drawn today.

## 7c. Interpreter anatomy (2026-08-19 evening, measured with cpi counters)

- **~60% of executed instructions are ARM**, not Thumb. FINDINGS Â§1's
  "Pokemon is Thumb-heavy" is wrong at runtime: gen-3's m4a sound engine
  mixes audio in ARM code from IWRAM every frame -- on real hardware that
  legitimately eats half the GBA CPU. The emulated workload is authentic.
- Cost per emulated instruction: **Thumb ~120 host cycles, ARM ~182** (BENCH
  cpiT/cpiA fields). Thumb handlers+CPULoop are IRAM; ARM's +60 is mostly
  its flash-resident handlers (248KB -- cannot fit IRAM wholesale).
- ARM dispatch-slot histogram is FLAT (top slot ~2.5%): no small hot handler
  set to cherry-pick into IRAM by slot. Aggregating slots by handler
  FUNCTION might still find a placeable top set -- untried.
- Dcache line 32B vs 64B: NO effect on emu fps. The interpreter is not
  line-fill-bound. -O3 on the core: cpiA WORSENED (183->191, icache bloat).
  Xtensa perfmon counters: dead on this chip/IDF combo (even forced PSRAM
  misses count 0) -- don't retry.
- Full-speed budget: ~2.4M insns/s needed; at current cpi that is ~360M
  cycles vs 240M available. cpi must drop ~35% for 60fps-with-audio on one
  core. Realistic candidates: per-function ARM handler IRAM placement,
  EWRAM (PSRAM) data locality, dispatch-loop register pinning.

## 8. Clock tree, measured (2026-08-19 late evening, clkprobe.c)

Boot-time probe: switch inside one flash-guard window (caches off, other CPU
stalled, everything IRAM -- `noinline` or GCC folds the IRAM function into its
flash caller and the first post-switch fetch wedges), measure ccount against
the raw systimer (XTAL-driven, PLL-immune), revert BEFORE re-enabling caches.
RTC-noinit breadcrumbs + a crash-once latch make wedges cost one WDT reset,
never a boot loop.

- **PLL320-mode CPUPERIOD_SEL=2 is NOT divide-by-1. It is a gated/illegal
  encoding: the CPU clock stops dead.** Wedges at stage 5 (divider write) at
  dbias +2 and +6 alike. 320-mode sel=1 (160 MHz) works fine -- the switch
  machinery is proven, the encoding is dead. The "320 MHz S3" is a myth on
  this silicon.
- **The BBPLL feedback divider is programmable and the VCO overdrives to
  ~557 MHz**: after stock 480 calibration, rewriting OC_DIV_7_0 (regi2c 0x66
  reg 3, mult-4) relocks at XTAL*mult. x13 -> 259 MHz CPU, x14 -> 278 MHz
  CPU (sel=2, /2). x15/x16 still measure 278: the VCO saturates, it does not
  wedge. Bias registers are identical between stock 320/480 configs, so no
  retuning needed up to saturation.
- **Max real CPU: 278 MHz (+16%).** Whole PLL tree scales: 80 MHz taps
  become 92.8 (flash/PSRAM/APB +16%), 48 MHz USB tap becomes 55.7 -> USB
  Serial-JTAG DEAD while held, including its virtual EN/BOOT reset lines.
  Physical reset only. FreeRTOS tick is systimer (XTAL) -- unaffected.
- Hold mode: one-shot NVS flag (OS_CMD_CLK320 0x07) cleared before the
  switch, so any reset lands back at stock 240. PSRAM hash stress after
  cache re-enable auto-reverts on mismatch. dbias bumped +2 steps over the
  chip's PVT 240M code while held.

## 9. The audio chain, actually fixed (2026-08-19 night)

Four stacked bugs, each masking the next. Mic-measured at every step
(webcam mic + 440 Hz band power vs silence baseline; user's ears confirmed
the final fix). The codec IS a genuine ES8311 (ID regs FD/FE = 83/11).

1. **Playback data was on the wrong pin.** Schematic nets are named from the
   codec's perspective: net I2S_DI (GPIO 8) wires to the codec's DSDIN =
   playback INPUT, net I2S_DO (GPIO 6) is ASDOUT = mic data. The ESP must
   TRANSMIT on GPIO 8. Confirmed electrically: routing-preserving pad-input
   sampling (IO_MUX FUN_IE + GPIO_IN_REG -- gpio_set_direction() would
   re-route the pad away from I2S and fake a dead line) shows MCK/SCK/LRC/DO
   all toggling.
2. **ES8311 reg01 was 0x30; must be 0x3F** -- internal DAC clock gates were
   closed. reg09/0A were 0x00 (24-bit SDP) against our 16-bit stream; must
   be 0x0C. Init is now a faithful port of Espressif's es8311 component
   (reset sequence + ratio-256 coeff row, valid at any absolute fs).
3. **Volume reg 0x32 is 0.5 dB/step, 0xBF = 0 dB.** The old pct*255/100
   mapping put "25%" at -64 dB and "max" at -38 dB -- physically playing,
   humanly inaudible; only a mic against the cone ever heard it.
4. **The SC8002B amp's AUDIO_EN is a SHUTDOWN pin: LOW = playing.** Freenove's
   own sketches hold it LOW forever; our "amp on last" HIGH kept it muted.

Plus: stutter at sub-full-speed is production-rate math, not a bug -- the
core makes 801.5 pairs per emulated frame, so at 30/60 speed a 47872 Hz
output starves every other frame. audioMatchRate() sets the I2S rate to
emuFps/59.73 of nominal each second (5% hysteresis, PLL-overdrive ratio
folded in): continuous slow-motion audio that converges to true pitch as
emulation speed rises.
