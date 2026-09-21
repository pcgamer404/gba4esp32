# AOT static recompile: ARM -> C -> Xtensa, flash XIP

The only known path to true 60fps-with-audio (FINDINGS Â§7c: interpreter needs
a ~35% cpi cut at 240 MHz, ~25% at 278). The S3 cannot JIT (no RWX PSRAM
aperture), but it can run unlimited precompiled code from flash XIP through
the icache. So: translate the ROM's hot code AT PACK TIME on the PC, compile
it into the firmware image (or a side partition), dispatch by PC hash at
runtime, fall back to the interpreter for everything untranslated.

## Why C, not Xtensa asm

Translate ARM/Thumb basic blocks into C functions operating on the existing
`bus.reg[]` CPU state. The xtensa-gcc we already ship does the register
allocation and scheduling; output quality is within ~15% of hand asm at 1% of
the effort, and the generated code is portable to the SDL port for testing.

## Pipeline

1. `tools/aot/scan.py` â€” walk the packed ROM (.pak) plus the IWRAM-copied
   regions (m4a mixer!). Static discovery: follow BL/BLX/B trees from the
   entry vector and the m4a SoundMain entry; mark basic blocks with start PC,
   mode (ARM/Thumb), and exit kind (fallthrough, branch, call, computed).
   The profiler's hot-PC histogram (add a BENCH dump of top interpreted PCs)
   picks which blocks are WORTH translating: target the top ~200KB of blocks
   covering ~90% of executed instructions.
2. `tools/aot/xlate.py` â€” per block, emit one C function
   `void aot_<pc>(void)` doing the ALU work on cached locals, writing flags
   only when a later instruction in the block reads them (flag liveness kills
   ~40% of the emitted work), calling the existing memory helpers
   (CPUReadMemory etc.) for loads/stores, and ending by setting
   `bus.armNextPC` + returning the consumed cycle count.
3. Generated output `components/esp_gba/main/aot_gen/<game>.c` (one file per game,
   selected by header game code at build time, or a side flash partition
   mmap'd XIP if multiple games must coexist).
4. Runtime: a `pcToFunc` open-addressed hash (PC>>1 -> fn ptr) checked in
   CPULoop before the interpreter dispatch. Miss = interpret as today.
   IWRAM code (m4a) is translated against its LOAD address; a version byte in
   the ROM header patch (same trick as the m4a downrate) invalidates if the
   game self-modifies (gen-3 doesn't, outside save-block trampolines).

## Correctness strategy

- The SDL port runs the same generated C: diff-test vs interpreter with
  lockstep state hashes per frame over scripted input (menu, battle, save).
- Blocks that touch PC explicitly, use LDM/STM with PC, or sit in
  self-modified pages are NOT translated (interpreter handles them).

## Expected gain

Interpreter cpi ~120 (Thumb) / ~182 (ARM). Translated blocks eliminate
dispatch (~20 cycles/insn), flag recompute (~15), and operand decode (~10);
memory ops keep their helper cost. Estimated cpi for translated code:
~45-60 => overall 2x+ once coverage passes ~80% of executed instructions.
At 278 MHz that clears the 60fps budget with margin for audio.

## Milestones

1. BENCH hot-PC histogram dump (firmware, ~1h) â€” sizing data.
2. scan.py block discovery on FireRed/Emerald, coverage report vs histogram.
3. xlate.py for the 20 most common ARM ALU/LDR/STR forms; SDL diff harness.
4. m4a mixer translated end-to-end (the single hottest stable target).
5. Full-game coverage pass, ESP integration, flash budget audit.
