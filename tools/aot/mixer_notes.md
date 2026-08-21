# m4a SoundMainRAM translation groundwork (Emerald J, live-verified)

Everything below was read from the RUNNING device (OS_CMD_PEEK), not docs.

- `SOUND_INFO_PTR` = 0x03007FF0 -> SoundInfo at **0x03006120**.
- SoundInfo layout confirmed against live bytes: ident "Smsh" (+0, +1 while
  the mixer holds its lock), pcmDmaCounter +4, reverb +5 (0x32 = ON in
  Emerald; the port now zeroes it every frame for the fast path),
  maxChans +6 (=5), masterVolume +7 (=12), freq index +8 (=2, our downrate
  patch), pcmSamplesPerVBlank +0x10 (=132), pcmFreq +0x14 (=7884),
  divFreq +0x18, CgbChans* +0x1c, then ROM func pointers (engine code lives
  at ROM ~0x0828d600+, NOT the 0x8006xxx cluster).
- **Mixer entry: 0x03001b51 (THUMB)**, called with r0 = SoundInfo. Referenced
  by ROM literals at 0x0828d658 (+1) / 0x0828e67c. Structure (matches pret
  m4a_1.s SoundMainRAM):
  - thumb: `ldrb r3,[r0,#5]` reverb check
    - reverb != 0: `adr r1,#4; bx r1` -> ARM reverb pass at 0x03001b5c
      (the ldrsb/ldrsb/add/mul/asr #9/strb loop over the DMA buffer,
      hot blocks 0x1b40/0x1b80)
    - reverb == 0: thumb zero-fill at 0x03001bb0 (stm burst loops)
  - then per-channel envelope state machine (thumb), ARM mix kernels at
    ~0x03001d40 and the helper `push {r4,ip}` function at 0x03001e44
    (hot blocks 0x1d40, 0x1e40, 0x1e80, 0x1ec0).
- Second IWRAM ARM blob at 0x03002800-0x03002940 = the game's interrupt
  handler (INTR_VECTOR 0x03007FFC -> 0x030027f0). Not a mixer concern.
- The hot ROM thumb cluster 0x08006600-0x08006b00 (19% of execution) is a
  SEPARATE translation target (sequencer/MPlayMain family; literal pool
  points at 0x03002398 = a RAM track struct).
- Raw dumps: scratchpad iwram_1800.bin (0x03001800+0x800) and
  iwram_2000.bin (0x03002000+0x800) from the session of 2026-08-20.

## Hook plan
`espgba_hle_pc = 0x03001b51`; in the THUMB dispatch, when armNextPC matches,
run the native mixer (reads SoundInfo like the ARM code does, via
internalRAM[(addr)&0x7FFF] / workRAM for sample pointers / rom for waves),
then set PC = LR (thumb), clobber r0-r3, charge ~sampleCount*channels*4
cycles. Keep an NVS kill-switch. Verify by ear + savegame soak; exact
per-sample math must follow the disassembly, not pret, where they differ.
