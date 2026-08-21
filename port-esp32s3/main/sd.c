/* SD card support for the Freenove FNK0104AB.
 *
 * The board wires a microSD slot to the SDMMC peripheral in 4-bit mode (not
 * SPI), on pins taken from the vendor's own sketch
 * (Tutorial_No_Touch/Sketches/Sketch_07.1_Music):
 *
 *   CLK 5, CMD 4, D0 6, D1 7, D2 2, D3 3
 *
 * None of these collide with the LCD (10/11/12/13/45/46) or the ES8311 audio
 * codec (1/15/16/17/18/21/38/39).
 */
#include "sd.h"

#include "os.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"  /* esp_rom_delay_us for the SD unwedge clock train */
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_partition.h"

#define TAG "SD"

/* SD pins come from os.h -- they differ per board variant. */

static sdmmc_card_t *card;
static bool mounted;
static bool sdMountOnce(void);

/* Can these pins even be driven and read?
 *
 * Distinguishes "the SDMMC peripheral is not talking" from "the pins are dead
 * or held by something else". CMD is driven low then released; with a card
 * present its internal pull-up (plus ours) should pull it back high.
 */
static void sdPinSanity(void) {
  const int pins[] = {SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, SD_PIN_D1, SD_PIN_D2,
                      SD_PIN_D3};
  const char *names[] = {"CLK", "CMD", "D0", "D1", "D2", "D3"};
  char idle[64] = {0};
  int p = 0;
  for (int i = 0; i < 6; i++) {
    gpio_reset_pin(pins[i]);
    gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    gpio_pullup_en(pins[i]);
  }
  vTaskDelay(pdMS_TO_TICKS(5));
  for (int i = 0; i < 6; i++) {
    p += snprintf(idle + p, sizeof(idle) - p, "%s=%d ", names[i],
                  gpio_get_level(pins[i]));
  }
  ESP_LOGI(TAG, "pin idle levels (all should read 1): %s", idle);

  /* Can each pin be driven BOTH ways? This separates "nothing is pulling the
   * line up" (pin drives high fine) from "the line is shorted low" (pin cannot
   * reach 1 even when driven). D0 reading 0 at idle is only a fault if it also
   * refuses to be driven high. */
  for (int i = 0; i < 6; i++) {
    /* INPUT_OUTPUT, not OUTPUT: plain output mode disables the input buffer, so
     * gpio_get_level() would read 0 for every pin regardless of what is on it
     * -- which is exactly the bogus "all pins shorted low" result that mode
     * produced here. */
    gpio_set_direction(pins[i], GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(pins[i], 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    int hi = gpio_get_level(pins[i]);
    gpio_set_level(pins[i], 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    int lo = gpio_get_level(pins[i]);
    gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    gpio_pullup_en(pins[i]);
    vTaskDelay(pdMS_TO_TICKS(2));
    int rel = gpio_get_level(pins[i]);
    gpio_pullup_dis(pins[i]);
    gpio_pulldown_en(pins[i]);
    vTaskDelay(pdMS_TO_TICKS(2));
    int pd = gpio_get_level(pins[i]);  /* 1 here => something drives it high */
    gpio_pulldown_dis(pins[i]);
    gpio_pullup_en(pins[i]);
    ESP_LOGI(TAG, "  %-3s drive1=%d drive0=%d pullup=%d pulldown=%d %s", names[i],
             hi, lo, rel, pd,
             hi == 0 ? "<-- SHORTED LOW"
                     : (rel == 0 ? "<-- floats low (no card pull-up?)" : "ok"));
  }

  /* Drive CMD low, then release and see whether it recovers. If it stays low,
   * something external is holding it; if it never went low, the pin is stuck. */
  gpio_set_direction(SD_PIN_CMD, GPIO_MODE_OUTPUT);
  gpio_set_level(SD_PIN_CMD, 0);
  vTaskDelay(pdMS_TO_TICKS(2));
  int low = gpio_get_level(SD_PIN_CMD);
  gpio_set_direction(SD_PIN_CMD, GPIO_MODE_INPUT);
  gpio_pullup_en(SD_PIN_CMD);
  vTaskDelay(pdMS_TO_TICKS(2));
  int rec = gpio_get_level(SD_PIN_CMD);
  ESP_LOGI(TAG, "CMD driven low reads %d, released reads %d (want 0 then 1)",
           low, rec);

  for (int i = 0; i < 6; i++) {
    gpio_reset_pin(pins[i]);
  }
}

/* SPI-mode fallback REMOVED. This slot is SDMMC-wired and SPI mode never once
 * mounted here, but a failed esp_vfs_fat_sdspi_mount() on IDF 4.4 corrupts the
 * heap on its cleanup path (see sdMount below). SPI2 belongs to the LCD. */

/* Last-resort recovery for a wedged card.
 *
 * A card interrupted mid-write keeps VDD (this board never cuts SD power)
 * and, crucially, receives NO CLOCK once the host gives up -- but an SD
 * card can only finish its internal program operation while being clocked.
 * So: hold every line low briefly (some controllers treat it as a bus
 * reset), then run a long 400kHz clock train with CMD/DAT released high,
 * exactly the state in which a busy card completes pending work and
 * returns to idle. Costs ~3.5s, only runs after a failed mount. */
static void sdUnwedge(void) {
  const gpio_num_t pins[6] = {SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0,
                              SD_PIN_D1,  SD_PIN_D2,  SD_PIN_D3};
  ESP_LOGW(TAG, "SD unwedge: bus low 250ms, then 400kHz clock train 3s");

  for (int i = 0; i < 6; i++) {
    gpio_reset_pin(pins[i]);
    gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    gpio_set_level(pins[i], 0);
  }
  vTaskDelay(pdMS_TO_TICKS(250));

  /* Release everything except CLK to pull-ups (idle-high bus). */
  for (int i = 1; i < 6; i++) {
    gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    gpio_pullup_en(pins[i]);
  }
  /* ~400kHz clock train, ~3s, with a watchdog-friendly yield every 50ms. */
  for (int burst = 0; burst < 60; burst++) {
    for (int c = 0; c < 20000; c++) {
      gpio_set_level(SD_PIN_CLK, 1);
      esp_rom_delay_us(1);
      gpio_set_level(SD_PIN_CLK, 0);
      esp_rom_delay_us(1);
    }
    vTaskDelay(1);
  }
  gpio_reset_pin(SD_PIN_CLK);
  for (int i = 0; i < 6; i++) {
    gpio_reset_pin(pins[i]);
  }
}

bool sdMount(void) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt == 1) {
      /* First failure: assume a wedged card and give it the clocks it
       * needs to finish whatever it was doing when the power stayed on
       * through a reset. */
      sdUnwedge();
    } else if (attempt > 1) {
      ESP_LOGW(TAG, "SD mount retry %d after 1500ms", attempt);
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
    if (sdMountOnce()) {
      return true;
    }
  }
  return false;
}

static bool sdMountOnce(void) {
  if (mounted) {
    return true;
  }

  sdPinSanity();

  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
      .format_if_mount_failed = false,  // never reformat a user's card
      .max_files = 4,
      .allocation_unit_size = 16 * 1024,
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  /* 400kHz was a debugging crutch while the mount was failing, and it made an
   * 8MB ROM copy take 105 seconds. The card negotiates down on its own if it
   * cannot keep up, so ask for high speed and let it decide. */
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = SD_PIN_CLK;
  slot.cmd = SD_PIN_CMD;
  slot.d0 = SD_PIN_D0;
  slot.d1 = SD_PIN_D1;
  slot.d2 = SD_PIN_D2;
  slot.d3 = SD_PIN_D3;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // boards often omit pull-ups

  esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount_cfg, &card);
  if (err != ESP_OK) {
    // Retry at 1-bit width: a card or wiring that cannot do 4-bit often still
    // enumerates on D0 alone, and that is worth knowing before blaming the card.
    // The host flag matters as well as the slot width; setting only slot.width
    // leaves the host still advertising 4-bit capability.
    ESP_LOGW(TAG, "SDMMC 4-bit failed (%s), retrying 1-bit", esp_err_to_name(err));
    slot.width = 1;
    host.flags = SDMMC_HOST_FLAG_1BIT;
    err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount_cfg, &card);
  }
  if (err != ESP_OK) {
    /* No SPI-mode fallback anymore. This slot is SDMMC-wired and SPI mode has
     * never once mounted here -- but its FAILURE path did real damage: on IDF
     * 4.4 a failed esp_vfs_fat_sdspi_mount() leaves the heap corrupted (buggy
     * cleanup; note the "SPI bus already initialized" on reattempts), which
     * later killed the UART lock and the SPI-LCD device struct at random.
     * Running it three times per boot (mount retries) made every no-card boot
     * a crash lottery. */
    ESP_LOGW(TAG, "SDMMC 1-bit failed (%s)", esp_err_to_name(err));
    return false;
  }

  mounted = true;
  ESP_LOGI(TAG, "mounted %s: %s %lluMB, %d-bit",
           SD_MOUNT, card->cid.name,
           ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024),
           slot.width);
  return true;
}

int sdListRoms(sdRomEntry *out, int max) {
  if (!mounted) {
    return 0;
  }
  DIR *dir = opendir(SD_ROM_DIR);
  if (!dir) {
    ESP_LOGE(TAG, "no %s directory on the card", SD_ROM_DIR);
    return 0;
  }

  int n = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) && n < max) {
    if (ent->d_name[0] == '.') {
      continue;
    }
    size_t len = strlen(ent->d_name);
    bool isGba = len >= 5 && strcasecmp(ent->d_name + len - 4, ".gba") == 0;
    bool isGbc = len >= 5 && strcasecmp(ent->d_name + len - 4, ".gbc") == 0;
    bool isGb = len >= 4 && strcasecmp(ent->d_name + len - 3, ".gb") == 0;
    if (!(isGba || isGbc || isGb)) {
      continue;
    }
    snprintf(out[n].name, sizeof(out[n].name), "%s", ent->d_name);

    // Sized from the entry buffer, not d_name's declared maximum, so the
    // compiler can prove there is no truncation.
    char path[sizeof(SD_ROM_DIR) + 1 + sizeof(out[n].name)];
    snprintf(path, sizeof(path), "%s/%s", SD_ROM_DIR, out[n].name);
    struct stat st;
    out[n].size = (stat(path, &st) == 0) ? (uint32_t)st.st_size : 0;

    /* Pull the real cartridge title and game code out of the GBA header
     * (0xA0 and 0xAC) rather than relying on the filename -- it is what the
     * per-game override table keys on, and it labels the icon accurately. */
    memset(out[n].title, 0, sizeof(out[n].title));
    memset(out[n].code, 0, sizeof(out[n].code));
    FILE *f = fopen(path, "rb");
    if (f) {
      uint8_t hdr[0xB0];
      if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
        memcpy(out[n].title, hdr + 0xA0, 12);
        memcpy(out[n].code, hdr + 0xAC, 4);
        for (int k = 0; k < 12; k++) {
          if (out[n].title[k] < 32 || out[n].title[k] > 126) out[n].title[k] = 0;
        }
      }
      fclose(f);
    }

    /* Raw size no longer decides playability: blank 64KB pages are dropped on
     * copy and aliased at mmap time, so a 16MB cart can occupy far less flash
     * (FireRed: 9.31MB). Anything up to the GBA maximum is a candidate; the
     * copy reports if it still does not fit after packing. */
    out[n].fits = out[n].size <= 32u * 1024 * 1024;
    n++;
  }
  closedir(dir);
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
