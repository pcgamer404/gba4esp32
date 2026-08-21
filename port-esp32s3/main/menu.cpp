/* Boot-time ROM picker.
 *
 * Draws into the same 240x160 framebuffer the emulator uses, so it goes through
 * the already-working blit path and needs no separate display code.
 *
 * Input is the BOOT button on GPIO 0 -- the only button this board has:
 *   short press  = move to next entry
 *   long press   = select (>600ms)
 *
 * GPIO 0 is a strapping pin, but only at reset; once running it is an ordinary
 * input with a pull-up.
 */
#include "menu.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <sys/stat.h>
#include <sys/types.h>
#include "os.h"
#include "sd.h"
#include "audio.h"
#include "clkprobe.h"
#include <stdlib.h>
#include <unistd.h>  /* unlink: drop a truncated pak cache */

extern "C" {
#include "vgafont8.h"
}

#define TAG "MENU"

#define BOOT_BTN ((gpio_num_t)0)
#define LONG_PRESS_MS 600

/* The UI uses the WHOLE panel; only the game keeps a border (scaling 240x160
 * up would need a scaler and cost frame rate, which is not worth it).
 *
 * FB is 76800 bytes = exactly 320x120, so a full 320x240 screen does not fit in
 * one buffer. It is rendered in two horizontal bands instead: every draw call
 * runs once per band and clips to it, then that band is blitted. Costs nothing
 * that matters for a static menu.
 */
#define MENU_W LCD_W
#define MENU_H LCD_H
#define FB_PIXELS (240 * 160)   /* mirrors main.cpp; FB is sized for the game */
#define BAND_H (FB_PIXELS / LCD_W)

static int bandY0 = 0;

extern uint16_t *FB;

/* FB holds big-endian RGB565 because the panel wants it that way. */
static inline uint16_t be(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static void menuClear(uint16_t colour) {
  uint16_t v = be(colour);
  for (int i = 0; i < MENU_W * BAND_H; i++) {
    FB[i] = v;
  }
}

/* Coordinates are full-screen; the band offset is applied here so callers never
 * have to know the screen is drawn in pieces. */
static void menuPix(int x, int y, uint16_t colour) {
  int by = y - bandY0;
  if (x < 0 || x >= MENU_W || by < 0 || by >= BAND_H) {
    return;
  }
  FB[x + by * MENU_W] = be(colour);
}

/* 8x8 VGA font, same layout upstream's ui.cpp uses. */
static void menuText(int x, int y, const char *s, uint16_t colour) {
  while (*s) {
    uint8_t c = (uint8_t)*s++;
    for (int row = 0; row < 8; row++) {
      uint8_t bits = vgafont8[c * 8 + row];
      for (int col = 0; col < 8; col++) {
        if (bits & (1 << (7 - col))) {
          menuPix(x + col, y + row, colour);
        }
      }
    }
    x += 8;
    if (x > MENU_W - 8) {
      return;  // clip rather than wrap; names are truncated by the caller
    }
  }
}

static void menuBar(int x, int y, int w, int h, uint16_t colour) {
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      menuPix(x + i, y + j, colour);
    }
  }
}

/* Blit the band currently in FB. */
static void menuFlushBand(void) {
  lcdWaitFB();
  lcdBlitRegion((uint8_t *)FB, 0, bandY0, MENU_W, BAND_H);
}

void menuFlush(void) { menuFlushBand(); }

/* Draw a whole full-screen view: run `draw` once per band, clipping to it. */
static void menuRender(void (*draw)(void)) {
  for (bandY0 = 0; bandY0 < MENU_H; bandY0 += BAND_H) {
    draw();
    menuFlushBand();
  }
  bandY0 = 0;
}

static const char *gMsg1, *gMsg2;
static void drawMessage(void) {
  menuClear(0x0000);
  menuText(12, MENU_H / 2 - 12, gMsg1, 0xFFFF);
  if (gMsg2) {
    menuText(12, MENU_H / 2 + 4, gMsg2, 0x7BEF);
  }
}

void menuMessage(const char *line1, const char *line2) {
  gMsg1 = line1;
  gMsg2 = line2;
  /* Always echo to serial: every failure in the copy path used to be LCD-only,
   * which made an unattended board loop forever with no clue in the log. */
  printf("MENU: %s%s%s\n", line1, line2 ? " -- " : "", line2 ? line2 : "");
  menuRender(drawMessage);
}

static const char *gProgLabel;
static int gProgPct;
static void drawProgress(void) {
  const int barW = MENU_W - 40;
  menuClear(0x0000);
  menuText(20, MENU_H / 2 - 26, gProgLabel, 0xFFFF);
  menuBar(20, MENU_H / 2 - 6, barW, 14, 0x18E3);
  int w = (barW * (gProgPct < 0 ? 0 : gProgPct > 100 ? 100 : gProgPct)) / 100;
  menuBar(20, MENU_H / 2 - 6, w, 14, 0x07E0);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", gProgPct);
  menuText(MENU_W / 2 - 12, MENU_H / 2 + 16, buf, 0xFFFF);
}

void menuProgress(const char *label, int pct) {
  gProgLabel = label;
  gProgPct = pct;
  if (pct % 10 == 0) {
    printf("MENU: %s %d%%\n", label, pct);  /* serial heartbeat for the host */
  }
  menuRender(drawProgress);
}

/* Returns: 0 = nothing, 1 = short press, 2 = long press. Blocks while held. */
static int buttonPoll(void) {
  if (gpio_get_level(BOOT_BTN) != 0) {
    return 0;
  }
  int held = 0;
  while (gpio_get_level(BOOT_BTN) == 0 && held < 4000) {
    vTaskDelay(pdMS_TO_TICKS(10));
    held += 10;
  }
  if (held < 30) {
    return 0;  // debounce
  }
  return held >= LONG_PRESS_MS ? 2 : 1;
}

void menuInitInput(void) {
  gpio_reset_pin(BOOT_BTN);
  gpio_set_direction(BOOT_BTN, GPIO_MODE_INPUT);
  gpio_pullup_en(BOOT_BTN);
}

/* ------------------------------------------------------------------ */
/* Icons                                                                */

/* Box art is optional: /sd/art/<rom name without .gba>.raw, ICON_W*ICON_H
 * RGB565 big-endian (same byte order as FB, so it blits with no conversion).
 * When absent a tile is generated from the cartridge's own game code, so the
 * library looks like a library without requiring the user to source artwork.
 */
static bool iconLoad(const char *romName, uint16_t *dst) {
  char base[256];
  snprintf(base, sizeof(base), "%s", romName);
  char *dot = strrchr(base, '.');
  if (dot) {
    *dot = 0;
  }
  char path[sizeof(SD_ART_DIR) + 262];
  snprintf(path, sizeof(path), "%s/%s.raw", SD_ART_DIR, base);

  FILE *f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  size_t want = (size_t)ICON_W * ICON_H * 2;
  size_t got = fread(dst, 1, want, f);
  fclose(f);
  return got == want;
}

/* Cheap deterministic hue from the game code, so each cart gets its own colour
 * and the same cart is always the same colour. */
static uint16_t iconTint(const char *code, int shade) {
  uint32_t h = 2166136261u;
  for (const char *p = code; *p; p++) {
    h = (h ^ (uint8_t)*p) * 16777619u;
  }
  int r = 6 + (int)((h >> 3) & 0x0F);
  int g = 6 + (int)((h >> 11) & 0x0F);
  int b = 6 + (int)((h >> 19) & 0x0F);
  r = (r * shade) / 16;
  g = (g * shade) / 16;
  b = (b * shade) / 16;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void iconGenerate(const sdRomEntry *rom, int x0, int y0) {
  /* Vertical gradient plate + the 4-char game code, with a lighter top edge so
   * it reads as a cartridge rather than a flat square. */
  for (int y = 0; y < ICON_H; y++) {
    int shade = 20 - (y * 8) / ICON_H;
    uint16_t c = iconTint(rom->code[0] ? rom->code : rom->name, shade);
    for (int x = 0; x < ICON_W; x++) {
      menuPix(x0 + x, y0 + y, c);
    }
  }
  for (int x = 0; x < ICON_W; x++) {
    menuPix(x0 + x, y0, 0xFFFF);
  }
  const char *code = rom->code[0] ? rom->code : "GBA";
  menuText(x0 + (ICON_W - (int)strlen(code) * 8) / 2, y0 + ICON_H / 2 - 4, code,
           0xFFFF);
}

static void iconDraw(const sdRomEntry *rom, int x0, int y0) {
  static uint16_t buf[ICON_W * ICON_H];
  if (iconLoad(rom->name, buf)) {
    for (int y = 0; y < ICON_H; y++) {
      for (int x = 0; x < ICON_W; x++) {
        /* Already big-endian on disk, so write raw rather than via menuPix
         * (which would byte-swap a second time). */
        int px = x0 + x, py = y0 + y - bandY0;
        if (px >= 0 && px < MENU_W && py >= 0 && py < BAND_H) {
          FB[px + py * MENU_W] = buf[x + y * ICON_W];  /* already big-endian */
        }
      }
    }
  } else {
    iconGenerate(rom, x0, y0);
  }

  if (!rom->fits) {
    /* Cross-hatch anything too big for the flash partition, so it is visibly
     * unplayable instead of failing only after you pick it. */
    for (int i = 0; i < ICON_W; i++) {
      menuPix(x0 + i, y0 + i * ICON_H / ICON_W, 0xF800);
      menuPix(x0 + ICON_W - 1 - i, y0 + i * ICON_H / ICON_W, 0xF800);
    }
  }
}


/* ------------------------------------------------------------------ */
/* Settings screen (gear icon in the picker). NVS-backed, navigable by   */
/* the button matrix: Up/Down select, A toggles, B leaves.               */

extern int showFps; /* main.cpp: border debug strip */

static uint8_t settingsGet(const char *key, uint8_t dflt) {
  nvs_handle_t h;
  uint8_t v = dflt;
  if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, key, &v);
    nvs_close(h);
  }
  return v;
}

static void settingsSet(const char *key, uint8_t v) {
  nvs_handle_t h;
  if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
  }
}

#define SET_ROWS 5 /* 3 toggles + volume + back */

static int gSetSel;
static void drawSettings(void) {
  menuClear(0x0000);
  menuText(16, 8, "SETTINGS", 0xFFFF);
  const char *names[3] = {"Debug overlay", "Overclock 260MHz", "Native audio mix"};
  uint8_t vals[3] = {settingsGet("dbg", 0), (uint8_t)(clkAutoGet() ? 1 : 0),
                     settingsGet("hle", 1)};
  for (int i = 0; i < 3; i++) {
    int y = 40 + i * 26;
    if (i == gSetSel) {
      menuBar(10, y - 4, MENU_W - 20, 22, 0x18E3);
    }
    menuText(24, y, names[i], 0xFFFF);
    menuText(MENU_W - 60, y, vals[i] ? "ON" : "OFF", vals[i] ? 0x07E0 : 0xF800);
  }
  int y = 40 + 3 * 26;
  if (gSetSel == 3) {
    menuBar(10, y - 4, MENU_W - 20, 22, 0x18E3);
  }
  menuText(24, y, "Volume", 0xFFFF);
  uint8_t vol = settingsGet("vol", AUDIO_DEFAULT_VOLUME_PCT);
  if (vol == 0) {
    menuText(MENU_W - 92, y, "MUTE", 0xF800);
  } else {
    char v[12];
    snprintf(v, sizeof(v), "< %3d%% >", vol);
    menuText(MENU_W - 92, y, v, 0x07E0);
  }
  y = 40 + 4 * 26;
  if (gSetSel == 4) {
    menuBar(10, y - 4, MENU_W - 20, 22, 0x18E3);
  }
  menuText(24, y, "Back", 0x7BEF);
  menuText(16, MENU_H - 20, "A toggle / left-right volume / B back", 0x39E7);
}

static void settingsToggle(int i) {
  if (i == 0) {
    uint8_t v = !settingsGet("dbg", 0);
    settingsSet("dbg", v);
    showFps = v;
  } else if (i == 1) {
    clkAutoSet(!clkAutoGet());
  } else if (i == 2) {
    settingsSet("hle", !settingsGet("hle", 1));
  } else if (i == 3) {
    /* A on the volume row = mute toggle, remembering the level. */
    uint8_t vol = settingsGet("vol", AUDIO_DEFAULT_VOLUME_PCT);
    if (vol > 0) {
      settingsSet("volp", vol);
      settingsSet("vol", 0);
    } else {
      settingsSet("vol", settingsGet("volp", AUDIO_DEFAULT_VOLUME_PCT));
    }
    audioSetVolume(settingsGet("vol", AUDIO_DEFAULT_VOLUME_PCT));
  }
}

static void settingsVolumeStep(int dir) {
  int vol = settingsGet("vol", AUDIO_DEFAULT_VOLUME_PCT) + dir * 10;
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  settingsSet("vol", (uint8_t)vol);
  audioSetVolume(vol);
}

static void menuSettings(void) {
  gSetSel = 0;
  menuRender(drawSettings);
  uint32_t lastKeys = 0xFFFFFFFF; /* force release before first press */
  while (1) {
    uint32_t k = osReadKey();
    uint32_t pressed = k & ~lastKeys;
    lastKeys = k;
    if (pressed & (1u << 6)) { /* up */
      gSetSel = (gSetSel + SET_ROWS - 1) % SET_ROWS;
      menuRender(drawSettings);
    }
    if (pressed & (1u << 7)) { /* down */
      gSetSel = (gSetSel + 1) % SET_ROWS;
      menuRender(drawSettings);
    }
    if (gSetSel == 3 && (pressed & ((1u << 5) | (1u << 4)))) { /* volume +/- */
      settingsVolumeStep((pressed & (1u << 4)) ? +1 : -1);
      menuRender(drawSettings);
    }
    if (pressed & (1u << 0)) { /* A */
      if (gSetSel == 4) return;
      settingsToggle(gSetSel);
      menuRender(drawSettings);
    }
    if (pressed & (1u << 1)) return; /* B = back */
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

#define GRID_COLS 5
#define GRID_ROWS 3
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)
#define CELL_W 62
#define CELL_H 70
#define GRID_X0 6
#define GRID_Y0 16

static void cellOrigin(int slot, int *x, int *y) {
  *x = GRID_X0 + (slot % GRID_COLS) * CELL_W + (CELL_W - ICON_W) / 2;
  *y = GRID_Y0 + (slot / GRID_COLS) * CELL_H;
}

static const sdRomEntry *gRoms;
static int gN, gSel;
static const char *gFlashed;

static void drawGear(void) {
  /* Bottom-right chip telling the player how to reach the settings. */
  int y = MENU_H - 18;
  menuBar(MENU_W - 132, y, 130, 16, 0x18E3);
  menuBar(MENU_W - 132, y, 130, 1, 0x7BEF);
  menuBar(MENU_W - 132, y + 15, 130, 1, 0x7BEF);
  menuText(MENU_W - 128, y + 4, "SELECT=SETTINGS", 0xFFFF);
}

static void drawLibrary(void) {
  const sdRomEntry *roms = gRoms;
  int n = gN, sel = gSel;
  const char *flashed = gFlashed;

  menuClear(0x0000);
  menuBar(0, 0, MENU_W, 11, 0x0010);
  menuText(3, 2, "esp-gba library", 0xFFFF);

  int page = sel / GRID_PER_PAGE;
  int first = page * GRID_PER_PAGE;
  int pages = (n + GRID_PER_PAGE - 1) / GRID_PER_PAGE;

  for (int slot = 0; slot < GRID_PER_PAGE; slot++) {
    int idx = first + slot;
    if (idx >= n) {
      break;
    }
    int x, y;
    cellOrigin(slot, &x, &y);

    if (idx == sel) {
      menuBar(x - 3, y - 3, ICON_W + 6, ICON_H + 6, 0xFFE0);  /* selection ring */
    }
    iconDraw(&roms[idx], x, y);

    /* Short label under each icon: the cartridge title beats the filename. */
    const char *label = roms[idx].title[0] ? roms[idx].title : roms[idx].name;
    char buf[8];
    snprintf(buf, sizeof(buf), "%.7s", label);
    uint16_t col = 0xFFFF;
    if (!roms[idx].fits) {
      col = 0xF800;
    } else if (flashed && strcmp(flashed, roms[idx].name) == 0) {
      col = 0x07E0;
    }
    menuText(x + (ICON_W - (int)strlen(buf) * 8) / 2, y + ICON_H + 2, buf, col);
  }

  /* Footer: full name of the selection, plus size and page. */
  menuBar(0, MENU_H - 21, MENU_W, 21, 0x0000);
  char line[31];
  snprintf(line, sizeof(line), "%.29s",
           roms[sel].title[0] ? roms[sel].title : roms[sel].name);
  menuText(3, MENU_H - 20, line, 0xFFFF);

  char foot[48];
  if (!roms[sel].fits) {
    snprintf(foot, sizeof(foot), "%.1fMB too big for flash",
             roms[sel].size / 1048576.0);
  } else {
    /* code is 4 chars; bound it explicitly so the compiler can size the result */
    snprintf(foot, sizeof(foot), "%.4s %.1fMB  pg %d/%d", roms[sel].code,
             roms[sel].size / 1048576.0, page + 1, pages);
  }
  menuText(3, MENU_H - 10, foot, roms[sel].fits ? 0x7BEF : 0xF800);

  /* Battery + charge state, left of the settings chip. "CHG" while a USB
   * host is powering the board, else the pack voltage. */
  {
    int mv = osBatteryMv();
    if (mv > 9990) mv = 9990;   /* bound %d so snprintf provably fits */
    if (mv < 0) mv = 0;
    char batt[16];
    if (osUsbPresent()) {
      snprintf(batt, sizeof(batt), "CHG %d.%02dV", mv / 1000,
               (mv % 1000) / 10);
    } else if (mv > 0) {
      snprintf(batt, sizeof(batt), "%d.%02dV", mv / 1000, (mv % 1000) / 10);
    } else {
      batt[0] = 0;
    }
    int w = (int)strlen(batt) * 8;
    menuText(LCD_W - 140 - w, MENU_H - 10, batt,
             osUsbPresent() ? 0x07E0 : mv < 3500 ? 0xF800 : 0x7BEF);
  }

  /* Last so nothing overdraws it. */
  drawGear();
}

static void drawList(const sdRomEntry *roms, int n, int sel, const char *flashed) {
  gRoms = roms;
  gN = n;
  gSel = sel;
  gFlashed = flashed;
  menuRender(drawLibrary);
}

int menuChooseRom(const sdRomEntry *roms, int n, const char *flashed) {
  if (n <= 0) {
    return -1;
  }
  /* Start highlighted on the game already in flash (instant resume), else the
   * first that fits. Nothing starts without a button or serial pick. */
  int sel = 0;
  if (flashed) {
    for (int i = 0; i < n; i++) {
      if (strcmp(flashed, roms[i].name) == 0) {
        sel = i;
        break;
      }
    }
  } else {
    for (int i = 0; i < n; i++) {
      if (roms[i].fits) {
        sel = i;
        break;
      }
    }
  }
  drawList(roms, n, sel, flashed);

  /* Refresh the footer's battery gauge every few seconds while idle. */
  int gaugeMs = 0;

  while (1) {
    /* Host-driven pick, so a benchmark can start a chosen game from reset
     * without a finger on the glass. */
    osPollSerial();
    gaugeMs += 20;
    if (gaugeMs >= 3000) {
      gaugeMs = 0;
      drawList(roms, n, sel, flashed);
    }
    int sp = osTakeSerialPick();
    if (sp >= 0 && sp < n) {
      ESP_LOGI(TAG, "serial pick: [%d] %s", sp, roms[sp].name);
      if (roms[sp].fits) {
        return sp;
      }
      ESP_LOGW(TAG, "serial pick [%d] does not fit flash, ignored", sp);
    }

    {
      static uint32_t lastK = 0xFFFFFFFF;
      uint32_t k = osReadKey();
      uint32_t pressed = k & ~lastK;
      lastK = k;
      int move = 0;
      if (pressed & (1u << 4)) move = 1;              /* right */
      if (pressed & (1u << 5)) move = -1;             /* left */
      if (pressed & (1u << 6)) move = -GRID_COLS;     /* up */
      if (pressed & (1u << 7)) move = GRID_COLS;      /* down */
      if (move) {
        gaugeMs = 0;
        int ns = sel + move;
        if (ns >= 0 && ns < n) sel = ns;
        drawList(roms, n, sel, flashed);
      }
      if (pressed & ((1u << 0) | (1u << 3))) {        /* A or Start = play */
        if (roms[sel].fits) return sel;
      }
      if (pressed & (1u << 2)) {                      /* Select = settings */
        gaugeMs = 0;
        menuSettings();
        drawList(roms, n, sel, flashed);
      }
    }

    int p = buttonPoll();
    if (p == 1) {
      gaugeMs = 0;
      do {
        sel = (sel + 1) % n;
      } while (!roms[sel].fits && n > 1);  /* skip carts that cannot run */
      drawList(roms, n, sel, flashed);
    } else if (p == 2) {
      if (roms[sel].fits) {
        return sel;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}


/* ------------------------------------------------------------------ */
/* Which ROM is currently in the flash partition                        */

#define NVS_NS "espgba"
#define NVS_KEY "romname"
#define NVS_KEY_CODE "romcode"
#define NVS_KEY_SIZE "romsize"
#define NVS_KEY_MAP "rommap"
#define NVS_KEY_PAGES "rompages"

#define ROM_PAGE 65536
#define ROM_MAX_PAGES 512

/* m4a/mp2k sound-engine downrate patch.
 *
 * Every GBA Pokemon carries the SoundMode word 0x0094C500 in its literal
 * pool: reverb 0, 5 DirectSound channels, master vol 12, freq index 4
 * (13379Hz), 8-bit DAC. The game's own ARM mixer -- measured at nearly half
 * of ALL emulated CPU work -- costs proportionally to that sample rate.
 * Rewriting the freq nibble while the ROM streams into flash cuts the mixer
 * load with zero emulator overhead. Index 2 = 7884Hz (~40% less mixing),
 * index 3 = 10512Hz (~21% less). The word is patched wherever it appears
 * (word-aligned literal pools only), any other content is untouched.
 */
#ifndef ESPGBA_M4A_FREQ_IDX
#define ESPGBA_M4A_FREQ_IDX 2
#endif
#define NVS_KEY_M4A "m4afreq"

static bool m4aPatchEnabled; /* .gba only: the magic literal could occur in
                              * arbitrary GB/GBC data */
static uint32_t m4aPatchPage(uint8_t *buf) {
  uint32_t hits = 0;
  if (!m4aPatchEnabled) return 0;
#if ESPGBA_M4A_FREQ_IDX != 4
  uint32_t *w = (uint32_t *)buf;
  for (uint32_t i = 0; i < ROM_PAGE / 4; i++) {
    if (w[i] == 0x0094C500u) {
      w[i] = 0x0090C500u | ((uint32_t)ESPGBA_M4A_FREQ_IDX << 16);
      hits++;
    }
  }
#endif
  return hits;
}

static uint32_t gRomSize;

bool romGetFlashed(char *out, size_t len) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    return false;
  }
  size_t sz = len;
  esp_err_t err = nvs_get_str(h, NVS_KEY, out, &sz);
  char code[8] = {0};
  size_t csz = sizeof(code);
  esp_err_t cerr = nvs_get_str(h, NVS_KEY_CODE, code, &csz);
  nvs_close(h);
  if (err != ESP_OK) {
    return false;
  }

  /* Verify against the flash itself, and FAIL CLOSED.
   *
   * An earlier version skipped the check when no game code was recorded, which
   * is exactly the case for entries written before this key existed. It then
   * reported "already flashed", skipped the copy, and the emulator mmap'd
   * whatever was at the new partition offset -- garbage header, instant panic
   * loop. Anything we cannot positively verify must be treated as empty.
   */
  if (cerr != ESP_OK) {
    ESP_LOGW(TAG, "no game code recorded for '%s' -- forcing a re-copy", out);
    out[0] = 0;
    return false;
  }

  /* Different m4a downrate setting than the flashed copy: re-copy so the
   * patch (or its removal) takes effect. */
  {
    nvs_handle_t mh;
    uint8_t cur = 4;  /* unpatched default if key absent */
    if (nvs_open(NVS_NS, NVS_READONLY, &mh) == ESP_OK) {
      nvs_get_u8(mh, NVS_KEY_M4A, &cur);
      nvs_close(mh);
    }
    if (cur != ESPGBA_M4A_FREQ_IDX) {
      ESP_LOGW(TAG, "m4a freq idx %d != build's %d -- forcing a re-copy", cur,
               ESPGBA_M4A_FREQ_IDX);
      out[0] = 0;
      return false;
    }
  }

  /* With sparse packing, partition offset 0 is the SHARED BLANK page (slot 0);
   * ROM page 0 -- and with it the header -- lives wherever the page map put it
   * (slot 1 in practice). Reading the header at partition offset 0xAC therefore
   * always saw 0xFF and re-flashed the same game on every single boot. Look the
   * slot up in the stored map instead; fall back to offset 0 for flat copies. */
  uint32_t hdrOff = 0xAC;
  {
    nvs_handle_t mh;
    if (nvs_open(NVS_NS, NVS_READONLY, &mh) == ESP_OK) {
      /* nvs_get_blob refuses a short buffer, so read the whole map. */
      static uint16_t map[ROM_MAX_PAGES];
      size_t sz = sizeof(map);
      if (nvs_get_blob(mh, NVS_KEY_MAP, map, &sz) == ESP_OK &&
          sz >= sizeof(uint16_t)) {
        hdrOff = (uint32_t)map[0] * ROM_PAGE + 0xAC;
      }
      nvs_close(mh);
    }
  }

  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "rom");
  uint8_t hdr[4] = {0};
  if (!part || esp_partition_read(part, hdrOff, hdr, 4) != ESP_OK ||
      memcmp(hdr, code, 4) != 0) {
    ESP_LOGW(TAG,
             "flash holds '%.4s' at 0x%lx, NVS says '%.4s' -- treating as empty",
             (const char *)hdr, (unsigned long)hdrOff, code);
    out[0] = 0;
    return false;
  }
  return true;
}

uint32_t romGetFlashedSize(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    return 0;
  }
  uint32_t v = 0;
  nvs_get_u32(h, NVS_KEY_SIZE, &v);
  nvs_close(h);
  return v;
}

static void romSetFlashed(const char *name, const char *code) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    return;
  }
  nvs_set_str(h, NVS_KEY, name);
  nvs_set_str(h, NVS_KEY_CODE, code);
  nvs_set_u32(h, NVS_KEY_SIZE, gRomSize);
  nvs_commit(h);
  nvs_close(h);
}

/* Copy a ROM from the SD card into the `rom` flash partition.
 *
 * The emulator addresses `rom` directly via esp_partition_mmap, so a ROM that
 * does not fit in PSRAM has to live in flash. ~8MB takes a while, hence the
 * progress bar; NVS records what is in there so switching back to a game
 * already flashed is instant.
 */
/* Copy a ROM into flash, skipping pages that are entirely 0xFF.
 *
 * GBA carts are padded out to a power-of-two size, and the padding is not just
 * at the end: FireRed has a 5.22MB hole at offset 0x6C7D38, and 44.5% of the
 * file is 0xFF. At 64KB page granularity that is 107 of 256 pages blank, so it
 * needs only 9.31MB of flash instead of 16MB -- comfortably inside the 13.94MB
 * partition.
 *
 * Every blank page is mapped to ONE shared physical page of 0xFF via
 * spi_flash_mmap_pages(), so the emulator still sees a flat, contiguous 16MB
 * ROM. No paging, no SD reads at runtime, no loss of speed.
 */
bool romCopyFromSd(const char *name, uint32_t size, const char *code) {
  clkFlashGuard(); /* flash writes ahead: force stock clock (no-op normally) */
  {
    const char *dot = strrchr(name, '.');
    m4aPatchEnabled = dot && strcasecmp(dot, ".gba") == 0;
  }

  gRomSize = size;
  uint32_t m4aHits = 0;
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "rom");
  if (!part) {
    menuMessage("no 'rom' partition", NULL);
    return false;
  }

  uint32_t nPages = (size + ROM_PAGE - 1) / ROM_PAGE;
  if (nPages > ROM_MAX_PAGES) {
    menuMessage("rom larger than 32MB", NULL);
    return false;
  }

  char path[sizeof(SD_ROM_DIR) + 260];
  snprintf(path, sizeof(path), "%s/%s", SD_ROM_DIR, name);
  FILE *f = fopen(path, "rb");
  if (!f) {
    menuMessage("cannot open rom", name);
    return false;
  }

  /* Packed-ROM cache on the SD card.
   *
   * Packing means reading all 16MB and testing every 64KB page for blankness.
   * Caching the result as /sd/packed/<name>.pak (written pages, back to back)
   * plus a .map (the page table) means a re-pick reads only the pages that
   * exist -- 9.31MB instead of 16MB for FireRed -- and skips the scan entirely.
   *
   * It does NOT avoid the flash write, which is the bulk of the time. Honest
   * gain is roughly the read+scan share, not a transformation.
   */
  uint8_t *buf = (uint8_t *)heap_caps_malloc(ROM_PAGE, MALLOC_CAP_SPIRAM);
  if (!buf) {
    buf = (uint8_t *)malloc(ROM_PAGE);
  }
  static uint16_t map[ROM_MAX_PAGES];
  if (!buf) {
    fclose(f);
    menuMessage("out of memory", NULL);
    return false;
  }

  /* Use the cache if it is present and consistent. */
  char pak[sizeof(SD_PACK_DIR) + 264], mapPath[sizeof(SD_PACK_DIR) + 264];
  snprintf(pak, sizeof(pak), "%s/%s.pak", SD_PACK_DIR, name);
  snprintf(mapPath, sizeof(mapPath), "%s/%s.map", SD_PACK_DIR, name);
  FILE *pf = fopen(pak, "rb");
  FILE *mf = pf ? fopen(mapPath, "rb") : NULL;
  bool cached = false;
  if (pf && mf) {
    if (fread(map, sizeof(uint16_t), nPages, mf) == nPages) {
      cached = true;
      ESP_LOGI(TAG, "using packed cache %s", pak);
    }
  }
  if (mf) fclose(mf);
  if (pf && !cached) {
    fclose(pf);
    pf = NULL;
  }

  /* Invalidate the NVS record BEFORE touching the flash. An interrupted or
   * failed write used to leave the previous game's name and page map in NVS
   * pointing at half-overwritten flash -- the emulator then mmap'd garbage
   * through a stale map. Anything that dies between here and the final
   * romSetFlashed() must read back as "nothing flashed". */
  {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_key(h, NVS_KEY);
      nvs_erase_key(h, NVS_KEY_CODE);
      nvs_erase_key(h, NVS_KEY_MAP);
      nvs_erase_key(h, NVS_KEY_PAGES);
      nvs_commit(h);
      nvs_close(h);
    }
  }

  uint32_t nextSlot = 1;
  uint32_t maxSlots = part->size / ROM_PAGE;
  int lastPct = -1;
  bool ok = true;
  esp_err_t werr = ESP_OK;

  /* Erase per 64KB page right before writing it, instead of the whole 14MB
   * partition up front. Cost is proportional to the game's real content, the
   * unused tail is never erased, and a failure part-way wastes less wear. */
  #define SLOT_PREP(slot)                                                     \
    ((werr = esp_partition_erase_range(part, (slot)*ROM_PAGE, ROM_PAGE)) == ESP_OK)

  /* Slot 0 is the shared blank page every 0xFF region will alias. Erased flash
   * already reads 0xFF, so erasing IS writing it. */
  menuProgress("preparing flash...", 0);
  if (!SLOT_PREP(0)) {
    free(buf);
    fclose(f);
    if (pf) fclose(pf);
    printf("MENU: erase slot 0 failed: %s\n", esp_err_to_name(werr));
    menuMessage("flash erase failed", esp_err_to_name(werr));
    return false;
  }

  if (cached) {
    /* Straight copy of the pre-packed pages; the map is already known. */
    uint32_t slots = 0;
    for (uint32_t i = 0; i < nPages; i++) {
      if (map[i] > slots) slots = map[i];
    }
    for (uint32_t sIdx = 1; sIdx <= slots; sIdx++) {
      size_t got = fread(buf, 1, ROM_PAGE, pf);
      if (got == ROM_PAGE) {
        m4aHits += m4aPatchPage(buf);
      }
      if (got != ROM_PAGE) {
        printf("MENU: pak read failed at slot %u: got %u of %u (eof=%d err=%d)"
               " -- cache truncated, deleting it\n",
               (unsigned)sIdx, (unsigned)got, (unsigned)ROM_PAGE, feof(pf),
               ferror(pf));
        ok = false;
        break;
      }
      if (!SLOT_PREP(sIdx) ||
          (werr = esp_partition_write(part, sIdx * ROM_PAGE, buf, ROM_PAGE)) !=
              ESP_OK) {
        printf("MENU: flash write failed at slot %u: %s\n", (unsigned)sIdx,
               esp_err_to_name(werr));
        ok = false;
        break;
      }
      int pct = (int)((uint64_t)sIdx * 100 / slots);
      if (pct != lastPct) {
        lastPct = pct;
        menuProgress("copying (cached)...", pct);
      }
    }
    fclose(pf);
    if (!ok) {
      /* A bad cache would fail identically on every retry; remove it so the
       * next attempt falls back to scanning the real ROM. */
      unlink(pak);
      unlink(mapPath);
    }
    nextSlot = slots + 1;
    goto finish;
  }

  /* No cache: scan and pack, writing the cache as we go. */
  mkdir(SD_PACK_DIR, 0777);
  pf = fopen(pak, "wb");
  mf = fopen(mapPath, "wb");

  for (uint32_t i = 0; i < nPages; i++) {
    size_t got = fread(buf, 1, ROM_PAGE, f);
    if (got == 0) {
      printf("MENU: rom read stopped at page %u/%u (eof=%d err=%d)\n",
             (unsigned)i, (unsigned)nPages, feof(f), ferror(f));
      break;
    }
    if (got < ROM_PAGE) {
      memset(buf + got, 0xFF, ROM_PAGE - got);
    }

    bool blank = true;
    for (uint32_t k = 0; k < ROM_PAGE; k += 4) {
      if (*(uint32_t *)(buf + k) != 0xFFFFFFFFu) {
        blank = false;
        break;
      }
    }

    if (blank) {
      map[i] = 0;  /* alias the shared blank page */
    } else {
      m4aHits += m4aPatchPage(buf);
      if (nextSlot >= maxSlots) {
        printf("MENU: out of flash at page %u: %u slots used\n", (unsigned)i,
               (unsigned)nextSlot);
        ok = false;
        break;
      }
      if (!SLOT_PREP(nextSlot) ||
          (werr = esp_partition_write(part, nextSlot * ROM_PAGE, buf,
                                      ROM_PAGE)) != ESP_OK) {
        printf("MENU: flash write failed at slot %u: %s\n", (unsigned)nextSlot,
               esp_err_to_name(werr));
        ok = false;
        break;
      }
      map[i] = (uint16_t)nextSlot++;
      if (pf) {
        fwrite(buf, 1, ROM_PAGE, pf);
      }
    }

    int pct = (int)((uint64_t)(i + 1) * 100 / nPages);
    if (pct != lastPct) {
      lastPct = pct;
      menuProgress("copying to flash...", pct);
    }
  }
  if (mf) {
    fwrite(map, sizeof(uint16_t), nPages, mf);
    fclose(mf);
  }
  if (pf) {
    fclose(pf);
  }

finish:
  free(buf);
  fclose(f);

  if (!ok) {
    /* The specific reason (short pak read, write error, out of slots) has
     * already gone to serial at the point of failure. */
    menuMessage("rom copy failed", "details on serial console");
    return false;
  }

  ESP_LOGI(TAG, "%s: %u pages, %u written (%.2f MB), %u blank aliased", name,
           (unsigned)nPages, (unsigned)nextSlot, nextSlot * ROM_PAGE / 1048576.0,
           (unsigned)(nPages - nextSlot + 1));

  printf("MENU: m4a downrate patch: %lu SoundMode word(s) -> freq idx %d\n",
         (unsigned long)m4aHits, ESPGBA_M4A_FREQ_IDX);

  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_blob(h, NVS_KEY_MAP, map, nPages * sizeof(uint16_t));
    nvs_set_u32(h, NVS_KEY_PAGES, nPages);
    nvs_set_u8(h, NVS_KEY_M4A, ESPGBA_M4A_FREQ_IDX);
    nvs_commit(h);
    nvs_close(h);
  }
  romSetFlashed(name, code);
  return true;
}

/* Rebuild the page list for spi_flash_mmap_pages(). Returns page count. */
uint32_t romGetPageMap(int *pagesOut, uint32_t maxPages) {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "rom");
  if (!part) {
    return 0;
  }
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    return 0;
  }
  uint32_t nPages = 0;
  nvs_get_u32(h, NVS_KEY_PAGES, &nPages);
  static uint16_t map[ROM_MAX_PAGES];
  size_t sz = sizeof(map);
  esp_err_t err = nvs_get_blob(h, NVS_KEY_MAP, map, &sz);
  nvs_close(h);
  if (err != ESP_OK || nPages == 0 || nPages > maxPages) {
    return 0;
  }

  uint32_t base = part->address / ROM_PAGE;  /* absolute flash page index */
  for (uint32_t i = 0; i < nPages; i++) {
    pagesOut[i] = (int)(base + map[i]);
  }
  return nPages;
}

/* On-screen FPS, drawn into the 240x160 GAME framebuffer after the byteswap so
 * it rides along with that frame's existing blit -- no extra SPI traffic.
 * Shows EMULATION speed / DRAWN frames: the adaptive frame drop makes these
 * very different numbers, and emu is the one that is gameplay speed. It is
 * also the only telemetry that exists during an overclock hold (USB dead). */
void menuOverlayFps(const char *text) {
  /* Drawn over the RUNNING GAME, whose buffer is pix with a 256px stride --
   * not the 240/320-wide menu layouts. */
  const int stride = 256;
  char buf[28];
  snprintf(buf, sizeof(buf), "%s", text);
  int len = (int)strlen(buf);
  for (int y = 0; y < 10; y++) {
    for (int x = 0; x < len * 8 + 4; x++) {
      int px = 240 - (len * 8 + 4) - 2 + x;
      if (px >= 0 && px < 240 && y < 160) FB[px + y * stride] = 0;
    }
  }
  for (int i = 0; i < len; i++) {
    uint8_t c = (uint8_t)buf[i];
    for (int row = 0; row < 8; row++) {
      uint8_t bits = vgafont8[c * 8 + row];
      for (int col = 0; col < 8; col++) {
        if (!(bits & (1 << (7 - col)))) continue;
        int px = 240 - (len * 8) - 2 + i * 8 + col;
        int py = 1 + row;
        if (px >= 0 && px < 240 && py < 160) FB[px + py * stride] = be(0x07E0);
      }
    }
  }
}
