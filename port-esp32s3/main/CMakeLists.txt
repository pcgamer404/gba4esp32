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
INCLUDE_DIRS "." "../../src" "../../libretro" "../../libretro-common/include"
)
target_compile_options(${COMPONENT_LIB} PRIVATE  -O2 -DFRONTEND_SUPPORTS_RGB565 -DPIX_BE_565 -DHAVE_STDINT_H -DINLINE=inline -DLSB_FIRST -DUSE_FRAME_SKIP -DUSE_TWEAKS=0 -DTILED_RENDERING -DTHREADED_RENDERER=1 )


# Headless build: ESP_GBA_HEADLESS=1 pio run
# Skips lcdInit() and all GPIO setup so the firmware drives no pins at all.
# Use when bringing up an unfamiliar board -- our ST7789 pinout (GPIO
# 10/11/12/13/14/21) is almost certainly wired to something else there, and
# clocking unknown pins can buzz an onboard amplifier or contend with an
# onboard chip's outputs.
if(ESP_GBA_HEADLESS OR DEFINED ENV{ESP_GBA_HEADLESS})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE HEADLESS NO_LCD_BLIT)
  message(STATUS "esp-gba: HEADLESS build (no LCD, no GPIO)")
endif()

# Board select: ESP_GBA_BOARD=FNK0104AB pio run
# Freenove FNK0104A/B, 2.8" ILI9341 320x240 landscape. Pin map comes from the
# vendor's TFT_eSPI setup header, and deliberately leaves the ES8311 audio
# codec (GPIO 1/15/16/17/18/21/38/39) and SDMMC (GPIO 2-7) alone.
if(DEFINED ENV{ESP_GBA_BOARD})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_$ENV{ESP_GBA_BOARD})
  message(STATUS "esp-gba: board $ENV{ESP_GBA_BOARD}")
endif()
