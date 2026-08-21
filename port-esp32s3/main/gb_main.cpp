/* Game Boy / Game Boy Color path, built on gnuboy (retro-go's core).
 *
 * Reuses the whole GBA infrastructure: the ROM was already copied to the
 * flash partition and mmap'd by main.cpp, audio goes through the same ES8311
 * path at the same sample rate, input is the same key bitmask, and the frame
 * limiter is the same sample-paced design. gnuboy renders 160x144 RGB565
 * big-endian straight into pix, which the panel DMA sends as-is. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "menu.h"
#include "os.h"

extern "C" {
#include "gnuboy.h"
}

extern uint16_t *pix; /* gba core global; linear 160x144 here */
extern int showFps;

#define GB_RATE 47872 /* same as the GBA path: the I2S chain is already set */
#define GB_SOUND_FRAMES 1024

static int16_t gbSound[GB_SOUND_FRAMES * 2];
static volatile uint32_t gbSamples; /* stereo frames produced */
static uint32_t gbFrames, gbBlits;

/* 1.5x nearest scale: 160x144 -> 240x216, nearly full screen. The scaled
 * frame (103KB) exceeds pix, so it goes out in three 72-line bands through
 * a band buffer placed in pix right after the 160x144 core framebuffer
 * (46080 + 34560 = 80640 <= 81920). Drawn frames run at 30Hz -- the whole
 * scaled frame costs ~14ms of SPI, too much for 60 -- while emulation stays
 * at 60; same cadence the GBA path uses. */
#define GB_OUT_W 240
#define GB_OUT_H 216
static uint16_t *gbBand;
static uint16_t gbXmap[GB_OUT_W];

static void gbVideoCb(void *buffer) {
  const uint16_t *src = (const uint16_t *)buffer;
  for (int band = 0; band < 3; band++) {
    /* The previous band's DMA reads gbBand: drain it BEFORE scaling into
     * the same buffer. Scaling first overwrote the in-flight transfer --
     * the "duplicated bottom" glitch. */
    lcdWaitFB();
    uint16_t *dst = gbBand;
    for (int oy = 0; oy < GB_OUT_H / 3; oy++) {
      int gy = band * (GB_OUT_H / 3) + oy;
      const uint16_t *sl = src + (gy * 2 / 3) * GB_WIDTH;
      for (int ox = 0; ox < GB_OUT_W; ox++) {
        dst[ox] = sl[gbXmap[ox]];
      }
      dst += GB_OUT_W;
    }
    lcdBlitRegionAsync((uint8_t *)gbBand, (LCD_W - GB_OUT_W) / 2,
                       (LCD_H - GB_OUT_H) / 2 + band * (GB_OUT_H / 3),
                       GB_OUT_W, GB_OUT_H / 3);
  }
  gbBlits++;
}

static void gbAudioCb(void *buffer, size_t frames) {
  audioWrite((const int16_t *)buffer, (int)frames * 2);
  gbSamples += frames;
}

extern "C" void gbRun(const uint8_t *romData, size_t romSize,
                      const char *name) {
  printf("GB: starting %s (%u bytes)\n", name, (unsigned)romSize);
  if (gnuboy_init(GB_RATE, GB_AUDIO_STEREO_S16, GB_PIXEL_565_BE, gbVideoCb,
                  gbAudioCb) < 0) {
    menuMessage("GB core init failed", NULL);
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }
  gnuboy_set_framebuffer(pix);
  gbBand = pix + GB_WIDTH * GB_HEIGHT;
  for (int ox = 0; ox < GB_OUT_W; ox++) {
    gbXmap[ox] = (uint16_t)(ox * 2 / 3);
  }
  gnuboy_set_soundbuffer(gbSound, GB_SOUND_FRAMES);

  if (gnuboy_load_rom(romData, romSize) < 0) {
    menuMessage("GB rom rejected", name);
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }
  gnuboy_reset(true);

  char sram[300];
  {
    char base[260];
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(sram, sizeof(sram), "/sd/saves/%s.srm", base);
  }
  gnuboy_load_sram(sram);

  int64_t limitBase = 0;
  uint32_t limitSamp = 0;
  uint32_t lastSamp = 0, lastBlit = 0;
  int64_t lastBench = esp_timer_get_time();
  int framesSinceSave = 0;

  while (1) {
    int req = osPollSerial();
    if (req & OS_REQ_SAVE) {
      gnuboy_save_sram(sram, false);
      printf("GB: sram saved to %s\n", sram);
    }

    uint32_t k = osReadKey();
    int pad = 0;
    if (k & (1u << 0)) pad |= GB_PAD_A;
    if (k & (1u << 1)) pad |= GB_PAD_B;
    if (k & (1u << 2)) pad |= GB_PAD_SELECT;
    if (k & (1u << 3)) pad |= GB_PAD_START;
    if (k & (1u << 4)) pad |= GB_PAD_RIGHT;
    if (k & (1u << 5)) pad |= GB_PAD_LEFT;
    if (k & (1u << 6)) pad |= GB_PAD_UP;
    if (k & (1u << 7)) pad |= GB_PAD_DOWN;
    gnuboy_set_pad(pad);

    gnuboy_run((gbFrames & 1) == 0); /* render every other frame: 30Hz draw */
    gbFrames++;

    /* Sample-paced limiter, same design as the GBA loop. */
    {
      int64_t now = esp_timer_get_time();
      uint32_t samp = gbSamples;
      int64_t target =
          limitBase + (int64_t)(samp - limitSamp) * 1000000 / GB_RATE;
      if (limitBase == 0 || now > target + 50000) {
        limitBase = now;
        limitSamp = samp;
      } else if (now < target) {
        while (target - esp_timer_get_time() > 12000) {
          vTaskDelay(1);
        }
        while (esp_timer_get_time() < target) {
        }
      }
    }

    /* Autosave battery-backed SRAM shortly after the game writes it. */
    if (++framesSinceSave >= 180) {
      framesSinceSave = 0;
      if (gnuboy_sram_dirty()) {
        gnuboy_save_sram(sram, true);
      }
    }

    int64_t now = esp_timer_get_time();
    if (now - lastBench >= 1000000) {
      uint32_t ds = gbSamples - lastSamp;
      uint32_t db = gbBlits - lastBlit;
      lastSamp = gbSamples;
      lastBlit = gbBlits;
      printf("GBBENCH emu=%u.%u draw=%u\n",
             (unsigned)(ds * 10 / (GB_RATE / 60) / 10),
             (unsigned)(ds * 10 / (GB_RATE / 60) % 10), (unsigned)db);
      lastBench = now;
    }
  }
}
