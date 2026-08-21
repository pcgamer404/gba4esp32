/* Save persistence: libretro_save_buf <-> /sd/saves/<rom>.sav
 *
 * The core emulates the cartridge's save chip entirely inside
 * libretro_save_buf (128KB flash + 8KB EEPROM = 0x22000 bytes; globals.h).
 * The libretro frontend persists that buffer for us on desktop; this port
 * never did, so every power cycle lost the game.
 *
 * Dirty detection: the vba-next fork compiled here has systemSaveUpdateCounter
 * commented out, so there is no core-side "save changed" signal. Instead the
 * buffer is checksummed on a wall-clock interval. Pokémon writes its save over
 * several emulated frames, so a flush happens only after the checksum has been
 * BOTH different from the last flushed state AND stable across two consecutive
 * checks -- otherwise a mid-write snapshot could persist a torn save.
 *
 * The flush itself is write-to-temp + rename, so a power cut during the write
 * leaves the previous good .sav intact.
 */
#include "save.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd.h"

#undef B0
#include "globals.h"

#define TAG "SAVE"

#define SAVE_CHECK_MS 3000  /* checksum cadence (wall clock) */

static char gPath[sizeof(SD_SAVE_DIR) + 268];
static char gTmp[sizeof(SD_SAVE_DIR) + 272];
static bool gEnabled;
static uint32_t gFlushedSum;   /* checksum of the last state written to SD */
static uint32_t gPrevSum;      /* checksum at the previous check */
static bool gPrevDiffers;      /* previous check differed from flushed state */
static TickType_t gLastCheck;

/* FNV-1a over the whole buffer, 32 bits at a time. 136KB from PSRAM every few
 * seconds is nothing next to the emulator's own PSRAM traffic. */
static uint32_t saveSum(void) {
  const uint32_t *p = (const uint32_t *)libretro_save_buf;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < LIBRETRO_SAVE_BUF_LEN / 4; i++) {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

void saveInit(const char *romName) {
  gEnabled = false;
  if (!libretro_save_buf) {
    ESP_LOGW(TAG, "no save buffer allocated");
    return;
  }
  char base[260];
  snprintf(base, sizeof(base), "%s", romName);
  char *dot = strrchr(base, '.');
  if (dot) {
    *dot = 0;
  }
  snprintf(gPath, sizeof(gPath), "%s/%s.sav", SD_SAVE_DIR, base);
  snprintf(gTmp, sizeof(gTmp), "%s/%s.tmp", SD_SAVE_DIR, base);

  FILE *f = fopen(gPath, "rb");
  if (f) {
    size_t got = fread(libretro_save_buf, 1, LIBRETRO_SAVE_BUF_LEN, f);
    fclose(f);
    printf("SAVE: loaded %u bytes from %s\n", (unsigned)got, gPath);
  } else {
    printf("SAVE: no existing save at %s\n", gPath);
  }

  gEnabled = true;
  gFlushedSum = saveSum();
  gPrevSum = gFlushedSum;
  gPrevDiffers = false;
  gLastCheck = xTaskGetTickCount();
}

bool saveFlush(void) {
  if (!gEnabled) {
    return false;
  }
  uint32_t sum = saveSum();
  FILE *f = fopen(gTmp, "wb");
  if (!f) {
    ESP_LOGE(TAG, "cannot open %s for writing", gTmp);
    return false;
  }
  size_t put = fwrite(libretro_save_buf, 1, LIBRETRO_SAVE_BUF_LEN, f);
  fclose(f);
  if (put != LIBRETRO_SAVE_BUF_LEN) {
    ESP_LOGE(TAG, "short write %u/%u to %s", (unsigned)put,
             (unsigned)LIBRETRO_SAVE_BUF_LEN, gTmp);
    unlink(gTmp);
    return false;
  }
  unlink(gPath);  /* FatFS rename does not overwrite */
  if (rename(gTmp, gPath) != 0) {
    ESP_LOGE(TAG, "rename %s -> %s failed", gTmp, gPath);
    return false;
  }
  gFlushedSum = sum;
  gPrevSum = sum;
  gPrevDiffers = false;
  printf("SAVE: flushed %u bytes to %s\n", (unsigned)LIBRETRO_SAVE_BUF_LEN,
         gPath);
  return true;
}

void saveTick(void) {
  if (!gEnabled) {
    return;
  }
  TickType_t now = xTaskGetTickCount();
  if ((now - gLastCheck) * portTICK_PERIOD_MS < SAVE_CHECK_MS) {
    return;
  }
  gLastCheck = now;

  uint32_t sum = saveSum();
  bool differs = sum != gFlushedSum;
  /* Flush only when changed AND unchanged since the previous check: a save
   * still being written by the game keeps moving the checksum, so we wait for
   * it to settle rather than persist half of it. */
  if (differs && gPrevDiffers && sum == gPrevSum) {
    saveFlush();
  } else {
    gPrevSum = sum;
    gPrevDiffers = differs;
  }
}
