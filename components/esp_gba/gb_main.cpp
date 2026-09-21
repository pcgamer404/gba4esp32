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

/* menuGameSettings() is implemented in menu.cpp. Return values are kept local
 * here so menu.h does not need to know about emulator-specific actions. */
extern int menuGameSettings(const char *gameName, int *speedIndex);

#define GAME_MENU_RESUME       0
#define GAME_MENU_SAVE_STATE1  1
#define GAME_MENU_LOAD_STATE1  2
#define GAME_MENU_SAVE_STATE2  3
#define GAME_MENU_LOAD_STATE2  4
#define GAME_MENU_SAVE         5
#define GAME_MENU_RESET        6
#define GAME_MENU_EXIT         7

#define GB_RATE 47872 /* same as the GBA path: the I2S chain is already set */
#define GB_SOUND_FRAMES 1024

static int16_t gbSound[GB_SOUND_FRAMES * 2];
static volatile uint32_t gbSamples; /* stereo frames produced */
static uint32_t gbFrames, gbBlits;

/* 1.0x / 1.5x / 2.0x / 2.5x.  Values are stored as tenths to avoid floats in
 * the frame limiter.  At every speed the renderer remains around 30Hz so the
 * existing 1.5x framebuffer scaler does not suddenly consume the whole bus. */
static const uint8_t gbSpeedX10[4] = {10, 15, 20, 25};

/* Full-screen nearest-neighbor scale: 160x144 -> 320x240.
 * The full output is larger than the free area after the Game Boy framebuffer,
 * so use six 40-line bands. Each band is 320*40 pixels = 25.6KB; together
 * with the 160x144 core framebuffer this stays within the existing 80KB pix
 * allocation. The LCD DMA is drained before reusing the band buffer. */
#define GB_OUT_W 320
#define GB_OUT_H 240
#define GB_BAND_H 40
#define GB_BANDS (GB_OUT_H / GB_BAND_H)
static uint16_t *gbBand;
static uint16_t gbXmap[GB_OUT_W];
static uint16_t gbYmap[GB_OUT_H];

static void gbVideoCb(void *buffer) {
  const uint16_t *src = (const uint16_t *)buffer;
  for (int band = 0; band < GB_BANDS; band++) {
    lcdWaitFB();
    uint16_t *dst = gbBand;
    for (int oy = 0; oy < GB_BAND_H; oy++) {
      int gy = band * GB_BAND_H + oy;
      const uint16_t *sl = src + gbYmap[gy] * GB_WIDTH;
      for (int ox = 0; ox < GB_OUT_W; ox++) {
        dst[ox] = sl[gbXmap[ox]];
      }
      dst += GB_OUT_W;
    }
    lcdBlitRegionAsync((uint8_t *)gbBand, 0, band * GB_BAND_H,
                       GB_OUT_W, GB_BAND_H);
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
    gbXmap[ox] = (uint16_t)(ox * GB_WIDTH / GB_OUT_W);
  }
  for (int oy = 0; oy < GB_OUT_H; oy++) {
    gbYmap[oy] = (uint16_t)(oy * GB_HEIGHT / GB_OUT_H);
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

  int gbSpeedIndex = 0;
  uint32_t drawEvery = 2;
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

    /* SELECT + START now opens an extensible in-game menu instead of exiting
     * immediately.  The menu owns the combo until it is released. */
    if ((k & (1u << 2)) && (k & (1u << 3))) {
      int result = menuGameSettings(name, &gbSpeedIndex);
      drawEvery = (uint32_t)(gbSpeedX10[gbSpeedIndex] / 5);
      if (drawEvery < 1) drawEvery = 1;
      const char *stateFile1 = nullptr;
      const char *stateFile2 = nullptr;
      char state1[300];
      char state2[300];
      {
        char base[260];
        snprintf(base, sizeof(base), "%s", name);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        snprintf(state1, sizeof(state1), "/sd/saves/%s.state1", base);
        snprintf(state2, sizeof(state2), "/sd/saves/%s.state2", base);
        stateFile1 = state1;
        stateFile2 = state2;
      }

      if (result == GAME_MENU_SAVE_STATE1) {
        int r = gnuboy_save_state(stateFile1);
        menuMessage(r == 0 ? "State 1 saved" : "State 1 save failed",
                    r == 0 ? name : "check SD card");
        vTaskDelay(pdMS_TO_TICKS(700));
      } else if (result == GAME_MENU_LOAD_STATE1) {
        int r = gnuboy_load_state(stateFile1);
        if (r == 0) {
          menuMessage("State 1 loaded", name);
          limitBase = 0;
          limitSamp = gbSamples;
          gbFrames = 0;
          framesSinceSave = 0;
        } else {
          menuMessage("State 1 not found", name);
        }
        vTaskDelay(pdMS_TO_TICKS(700));
      } else if (result == GAME_MENU_SAVE_STATE2) {
        int r = gnuboy_save_state(stateFile2);
        menuMessage(r == 0 ? "State 2 saved" : "State 2 save failed",
                    r == 0 ? name : "check SD card");
        vTaskDelay(pdMS_TO_TICKS(700));
      } else if (result == GAME_MENU_LOAD_STATE2) {
        int r = gnuboy_load_state(stateFile2);
        if (r == 0) {
          menuMessage("State 2 loaded", name);
          limitBase = 0;
          limitSamp = gbSamples;
          gbFrames = 0;
          framesSinceSave = 0;
        } else {
          menuMessage("State 2 not found", name);
        }
        vTaskDelay(pdMS_TO_TICKS(700));
      } else if (result == GAME_MENU_SAVE) {
        int r = gnuboy_save_sram(sram, false);
        menuMessage(r == 0 ? "Game saved" : "Game save failed",
                    r == 0 ? name : "check SD card");
        vTaskDelay(pdMS_TO_TICKS(700));
      } else if (result == GAME_MENU_RESET) {
        gnuboy_reset(true);
        limitBase = 0;
        limitSamp = gbSamples;
        gbFrames = 0;
        framesSinceSave = 0;
      } else if (result == GAME_MENU_EXIT) {
        gnuboy_save_sram(sram, false);
        menuMessage("saving...", "returning to menu");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
      }
      continue;
    }

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

    gnuboy_run((gbFrames % drawEvery) == 0);
    gbFrames++;

    /* Sample-paced limiter.  Scaling the target interval by speed gives the
     * emulator 1.0x, 1.5x, 2.0x or 2.5x while leaving the core untouched. */
    {
      int64_t now = esp_timer_get_time();
      uint32_t samp = gbSamples;
      uint32_t speedX10 = gbSpeedX10[gbSpeedIndex];
      int64_t elapsedUs =
          ((int64_t)(samp - limitSamp) * 10000000LL) /
          ((int64_t)GB_RATE * speedX10);
      int64_t target = limitBase + elapsedUs;
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
