idf_component_register(SRCS "main.cpp"
"os.c"
"gbaover.cpp"
"sd.c"
"menu.cpp"
"audio.c"
"save.cpp"
"clkprobe.c"
"gb_main.cpp"
"hotpc.c"
"thread_esp.c"
INCLUDE_DIRS "." "../gba"
)
target_compile_options(${COMPONENT_LIB} PRIVATE  -O2 -DFRONTEND_SUPPORTS_RGB565 -DPIX_BE_565 -DHAVE_STDINT_H -DINLINE=inline -DLSB_FIRST -DUSE_FRAME_SKIP -DUSE_TWEAKS=0 -DTILED_RENDERING -DTHREADED_RENDERER=1 )


# Headless build: ESP_GBA_HEADLESS=1 pio run
# Skips LCD initialization and GPIO setup so the firmware drives no pins.
# Useful for emulator/core testing without interacting with the hardware.

if(ESP_GBA_HEADLESS OR DEFINED ENV{ESP_GBA_HEADLESS})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE HEADLESS NO_LCD_BLIT)
  message(STATUS "esp-gba: HEADLESS build (no LCD, no GPIO)")
endif()
