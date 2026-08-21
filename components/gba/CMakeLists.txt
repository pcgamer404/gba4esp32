# Mirror of port-esp32s3/components/gba/CMakeLists.txt with paths rebased.
# PlatformIO scans ${PROJECT_DIR}/components, which is the repo root here, so
# the emulator core sits two levels up instead of upstream's three.
idf_component_register(SRCS
    "../../src/gba.cpp"
    "../../src/memory.cpp"
    "../../src/sound.cpp"
    "../../src/system.cpp"
  INCLUDE_DIRS "." "../../src" "../../libretro" "../../libretro-common/include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -DFRONTEND_SUPPORTS_RGB565
    -DPIX_BE_565
    -DHAVE_STDINT_H
    -DINLINE=inline
    -DLSB_FIRST
    -DUSE_FRAME_SKIP
    # 44vba's own speed tweaks, shipped off in this fork: idle-loop burning on
    # taken BEQ, fixed gamepak waitstates, fast DMA fill/copy. Known safe on
    # Pokemon gen 3 (the watch port runs them).
    -DTILED_RENDERING
    -DUSE_TWEAKS=1
    -DTHREADED_RENDERER=1)
