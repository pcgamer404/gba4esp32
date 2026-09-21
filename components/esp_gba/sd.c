#include "sd.h"

#include "os.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define TAG "SD"

static sdmmc_card_t *card = NULL;
static bool mounted = false;

static bool sdMountOnce(void) {
  if (mounted) {
    return true;
  }

  /*
   * The LCD already initialized SPI2 in os.c.
   * SD attaches to that existing SPI2 bus as another SPI device.
   */
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_SPI_HOST;

  /*
   * SD-over-SPI should stay at the IDF default SD SPI frequency.
   * Do not use the LCD's 40 MHz setting for the SD card.
   */
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  /*
   * SPI SD device configuration.
   * No card-detect or write-protect GPIOs are used.
   */
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id = SD_SPI_HOST;
  slot.gpio_cs = SD_PIN_CS;

  /*
   * Keep CS inactive before the SD driver starts talking to the card.
   */
  gpio_reset_pin(SD_PIN_CS);
  gpio_set_direction(SD_PIN_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(SD_PIN_CS, 1);

  esp_vfs_fat_mount_config_t mount_cfg = {
      .format_if_mount_failed = false,
      .max_files = 4,
      .allocation_unit_size = 16 * 1024,
  };

  ESP_LOGI(TAG, "Mounting SD card over SPI2");
  ESP_LOGI(TAG, "SPI2: CLK=%d MOSI=%d MISO=%d CS=%d",
           SD_PIN_CLK,
           SD_PIN_MOSI,
           SD_PIN_MISO,
           SD_PIN_CS);

  esp_err_t err = esp_vfs_fat_sdspi_mount(
      SD_MOUNT,
      &host,
      &slot,
      &mount_cfg,
      &card);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SD mount failed: %s (0x%x)",
             esp_err_to_name(err), err);
    card = NULL;
    return false;
  }

  mounted = true;

  ESP_LOGI(TAG,
           "mounted %s: %s %lluMB",
           SD_MOUNT,
           card->cid.name,
           ((uint64_t)card->csd.capacity * card->csd.sector_size) /
               (1024 * 1024));

  return true;
}

bool sdMount(void) {
  return sdMountOnce();
}

int sdListRoms(sdRomEntry *out, int max) {
  if (!mounted || !out || max <= 0) {
    return 0;
  }

  /* Create the standard ROM folders automatically. */
  mkdir(SD_ROM_DIR, 0777);
  mkdir(SD_ROM_GB_DIR, 0777);
  mkdir(SD_ROM_GBC_DIR, 0777);
  mkdir(SD_ROM_GBA_DIR, 0777);

  const char *romDirs[] = {
      SD_ROM_GB_DIR,
      SD_ROM_GBC_DIR,
      SD_ROM_GBA_DIR,
  };

  int n = 0;

  for (int di = 0;
       di < (int)(sizeof(romDirs) / sizeof(romDirs[0])) && n < max;
       di++) {

    DIR *dir = opendir(romDirs[di]);
    if (!dir) {
      ESP_LOGW(TAG, "could not open %s", romDirs[di]);
      continue;
    }

    struct dirent *ent;

    while ((ent = readdir(dir)) && n < max) {
      if (ent->d_name[0] == '.') {
        continue;
      }

      size_t len = strlen(ent->d_name);

      bool isGba =
          len >= 5 && strcasecmp(ent->d_name + len - 4, ".gba") == 0;

      bool isGbc =
          len >= 5 && strcasecmp(ent->d_name + len - 4, ".gbc") == 0;

      bool isGb =
          len >= 4 && strcasecmp(ent->d_name + len - 3, ".gb") == 0;

      /* Each folder accepts only its matching ROM type. */
      if ((di == 0 && !isGb) ||
          (di == 1 && !isGbc) ||
          (di == 2 && !isGba)) {
        continue;
      }

      snprintf(out[n].name,
               sizeof(out[n].name),
               "%s",
               ent->d_name);

      char path[320];

      snprintf(path,
               sizeof(path),
               "%s/%s",
               romDirs[di],
               out[n].name);

      struct stat st;

      out[n].size =
          (stat(path, &st) == 0)
              ? (uint32_t)st.st_size
              : 0;

      /*
       * Only GBA ROMs have the GBA title/game-code header.
       * GB/GBC use the filename as their title and a platform code
       * for the generated icon.
       */
      memset(out[n].title, 0, sizeof(out[n].title));
      memset(out[n].code, 0, sizeof(out[n].code));

      if (isGba) {
        FILE *f = fopen(path, "rb");

        if (f) {
          uint8_t hdr[0xB0];

          if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
            memcpy(out[n].title, hdr + 0xA0, 12);
            memcpy(out[n].code, hdr + 0xAC, 4);

            for (int k = 0; k < 12; k++) {
              if (out[n].title[k] < 32 ||
                  out[n].title[k] > 126) {
                out[n].title[k] = 0;
              }
            }
          }

          fclose(f);
        }
      } else if (isGbc) {
        snprintf(out[n].code, sizeof(out[n].code), "GBC");
      } else {
        snprintf(out[n].code, sizeof(out[n].code), "GB");
      }

      out[n].fits =
          out[n].size <= 32u * 1024 * 1024;

      n++;
    }

    closedir(dir);
  }

  return n;
}

bool sdEnsureSaveDir(void) {
  if (!mounted) {
    return false;
  }

  struct stat st;

  if (stat(SD_SAVE_DIR, &st) == 0) {
    return true;
  }

  if (mkdir(SD_SAVE_DIR, 0777) == 0) {
    ESP_LOGI(TAG, "created %s", SD_SAVE_DIR);
    return true;
  }

  ESP_LOGW(TAG, "could not create %s", SD_SAVE_DIR);
  return false;
}