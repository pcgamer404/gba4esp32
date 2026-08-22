
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp32s3/rom/cache.h"  /* Cache_Dbus_MMU_Set: cart tail mapping */
#include "soc/cache_memory.h"   /* MMU_ACCESS_FLASH */
#include "os.h"
#include "clkprobe.h"
#include "sd.h"
#include "menu.h"
#include "audio.h"
#include "save.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define TAG "MAIN"

#undef B0
#include "gba.h"
#include "globals.h"
#include "memory.h"  // flashSize, flashSetSize, rtcEnable
#include "sound.h"   // soundSetSampleRate, soundReset

/* One framebuffer: `pix` itself.
 *
 * The core now emits panel byte order at the final pixel write (PIX_BE_565 in
 * port.h), so the old chain -- renderer writes pix in PSRAM, systemDrawScreen
 * byteswaps all 38400 pixels into an internal FB, SPI DMA reads FB -- collapses
 * to: renderer writes pix in internal SRAM, SPI DMA reads pix. That deletes a
 * full-frame PSRAM read + internal write per drawn frame AND moves every
 * scanline the renderer produces from PSRAM into internal SRAM, the second-
 * biggest placement win in gpsp-reload's measured table.
 *
 * pix is 256x160 u16 (stride 256, the sprite-overdraw padding vba-next needs).
 * The panel window is opened 256 wide so one contiguous DMA covers it; the
 * 16 padding columns are blanked each frame and land in the black border.
 *
 * FB aliases pix for the menu, which wants one 320x120 band buffer (76800B <=
 * 81920B). Menu and game never run concurrently.
 */
#define FB_PIXELS (240 * 160)
#define FB_BYTES (FB_PIXELS * 2)
#define PIX_STRIDE 256
#define PIX_BYTES (2 * PIX_STRIDE * 160)
uint16_t *FB;
int frameDrawn = 0;
uint32_t frameCount = 0;
uint32_t blitCount = 0;  /* frames that actually reached the panel */
int showFps = 1;      /* on-screen counter */
static uint8_t hleWanted = 1; /* "ui"/"hle" setting: arm the native mixer */
int lastDrawFps = 0;
int lastEmuFps = 0; /* whole fps; the overlay shows emu/drawn side by side */
/* On-glass telemetry: emu/drawn cpu%% wait%% arm%% -- the only BENCH that
 * exists during a 278 MHz hold (USB dead). Rebuilt every BENCH second and
 * drawn as a strip in the border ABOVE the game window (the game's own
 * pixels stay untouched). Tap the top border to toggle. */
static char overlayText[40] = "";
extern "C" {
#include "vgafont8.h"
}
static uint16_t dbgStrip[240 * 10];
static int batteryMv = -1;
static int usbPowered = 0;
static void dbgStripDraw(void) {
  memset(dbgStrip, 0, sizeof(dbgStrip));
  const char *t = overlayText;
  int x = 1;
  for (const char *c = t; *c && x < 200; c++, x += 6) {
    for (int row = 0; row < 8; row++) {
      uint8_t bits = vgafont8[(uint8_t)*c * 8 + row];
      for (int col = 0; col < 6; col++) {
        if (bits & (0x80 >> col)) dbgStrip[(row + 1) * 240 + x + col] = 0xFFFF;
      }
    }
  }
  /* battery glyph, right-aligned: outline + 0-4 bars (3.3V empty, 4.2 full);
   * reads >= 4500mV mean USB power, drawn as full */
  int bars = batteryMv < 0 ? 0
             : batteryMv > 4400 ? 4
             : batteryMv <= 3300 ? 0
             : (batteryMv - 3300) * 4 / 900 + 1;
  if (bars > 4) bars = 4;
  int bx = 240 - 24;
  for (int row = 2; row < 9; row++) {
    for (int col = 0; col < 20; col++) {
      bool edge = row == 2 || row == 8 || col == 0 || col == 19;
      if (edge) dbgStrip[row * 240 + bx + col] = 0x07E0;
    }
  }
  for (int b = 0; b < bars; b++) {
    for (int row = 4; row < 7; row++) {
      for (int col = 0; col < 3; col++) {
        dbgStrip[row * 240 + bx + 2 + b * 4 + col] = 0x07E0;
      }
    }
  }
  /* Charging: yellow "+" left of the battery while a USB host is present. */
  if (usbPowered) {
    for (int row = 2; row < 9; row++) dbgStrip[row * 240 + bx - 5] = 0xFFE0;
    for (int col = -8; col < -1; col++) dbgStrip[5 * 240 + bx + col] = 0xFFE0;
  }
  /* byteswap for the panel */
  for (int i = 0; i < 240 * 10; i++) {
    dbgStrip[i] = (uint16_t)((dbgStrip[i] >> 8) | (dbgStrip[i] << 8));
  }
}

extern bool fs_draw;  /* gba.cpp: did the PPU render this frame's scanlines? */

void emuRunFrame() {
  frameDrawn = 0;
  while (!frameDrawn) {
    CPULoop();
  }
  frameCount++;
}

/* Called by the emulator core when a game keeps jumping into unmapped
 * memory: the pack in flash is untrustworthy (bad copy, damaged source).
 * Forget it and reboot to the picker; the next pick re-packs from SD. */
extern "C" void espgba_on_bad_cart(void) {
  printf("BADCART: repeated bad jumps -- invalidating the flash pack and "
         "rebooting to the menu (next pick re-packs from SD)\n");
  romInvalidateFlashed();
  delayMS(200);
  esp_restart();
}

void systemMessage(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  ESP_LOGE("GBA", "%s", buf);
}

void systemDrawScreen(void) {
  frameDrawn = 1;

  /* The core calls this every emulated frame, rendered or not -- frameskip
   * only gates the PPU scanline work. Blitting a frame the PPU never rendered
   * just re-sends stale pixels, so skip the whole SPI path for those. */
  if (!fs_draw) {
    return;
  }

#ifndef NO_LCD_BLIT
  // The previous frame's DMA blit may still be in flight. Reclaim it before
  // queuing the next one -- by now the transfer has had a whole frame of
  // emulation to finish, so in practice this returns immediately.
  lcdWaitFB();

  /* The core wrote panel-ready big-endian RGB565 straight into pix
   * (PIX_BE_565); there is nothing left to convert or copy. Blank the 16
   * sprite-overdraw padding columns (240..255) so they show as border, then
   * DMA the whole 256-wide buffer in one queued transfer.
   *
   * Renderer-vs-DMA overlap: the emulator can start writing scanline 0 of the
   * next drawn frame while the tail of this transfer is still in flight. With
   * frameskip >= 1 the next PPU write to pix is a full skipped frame away,
   * far longer than the ~13ms the transfer needs; at frameskip 0 the worst
   * case is a partial-frame tear, never corruption. */
  for (int y = 0; y < 160; y++) {
    memset(pix + y * PIX_STRIDE + 240, 0, (PIX_STRIDE - 240) * 2);
  }
  lcdBlitRegionAsync((uint8_t *)pix, GBA_X_OFF, GBA_Y_OFF, PIX_STRIDE, 160);
#else
  (void)overlayText;
#endif
  blitCount++;
}

/* Audio capture.
 *
 * Upstream left this an empty stub, so the core's audio was generated and
 * thrown away. `length` is a count of int16 values (stereo interleaved), which
 * is at most 1600 per emulated frame at 47782 Hz -- exactly one frame's worth.
 * Buffer generously and drop the overflow rather than corrupting memory.
 */
#define AUDIO_CAP 4096
static int16_t audioBuf[AUDIO_CAP];
static volatile int audioLen = 0;  // in int16 units
static volatile uint32_t audioCallbacks = 0;
static volatile uint32_t audioSamples = 0;  // stereo pairs
static volatile uint32_t audioDropped = 0;

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {
  audioCallbacks++;
  // Stereo pairs, the only unambiguous measure of emulated time: this callback
  // fires more than once per GBA frame, so counting callbacks overstates the
  // frame rate by ~1.7x. samples / 47872 = seconds of emulated game time.
  audioSamples += (uint32_t)(length / 2);
  if (length <= 0) return;
  if (audioLen + length > AUDIO_CAP) {
    audioDropped += length;
    return;
  }
  memcpy(audioBuf + audioLen, finalWave, length * sizeof(int16_t));
  audioLen += length;
  audioWrite(finalWave, length);
}

void load_image_preferences(void);  // gbaover.cpp

void emuInit() {
  // Mirrors port-sdl2/main.cpp. Upstream's ESP32 emuInit() did only
  // CPUSetupBuffers/CPUInit/CPUReset/SetFrameskip, omitting the per-game
  // overrides, RTC and flash-size setup. Pokemon Gen 3 probes its save chip and
  // RTC during boot and hangs forever in forced blank without them.
  cpuSaveType = 0;
  flashSize = 0x10000;
  enableRtc = false;
  mirroringEnable = false;

  CPUSetupBuffers();
  CPUInit(NULL, false);
  load_image_preferences();  // keys off the ROM header game code
  CPUReset();

#if THREADED_RENDERER
  /* Launch the scanline renderer on core 1. The frontend is responsible for
   * this -- the libretro port calls it and upstream's ESP32 port never did,
   * which is why enabling THREADED_RENDERER alone just stalled: the emulator
   * waits for a renderer that was never started. */
  ThreadedRendererStart();
#endif

  soundSetSampleRate(47782);
  soundReset();
  rtcEnable(true);
  flashSetSize(flashSize);

  /* No frameskip: every emulated frame is rendered. The PPU cost that made
   * this untenable on one core now lives on core 1 (THREADED_RENDERER). */
  SetFrameskip(0);
}

extern "C" void app_main() {
  /* pix must be INTERNAL and DMA-capable: it is now the SPI source directly,
   * and spi_device_queue_trans() with a PSRAM source hangs on IDF 4.4 --
   * the transaction never completes and lcdWaitFB() blocks forever. Measured,
   * not assumed. 80KB fits because the old separate FB (76.8KB) is gone.
   */
  pix = (uint16_t *)heap_caps_malloc(PIX_BYTES,
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (pix == NULL) {
    printf("FATAL: no internal DMA memory for pix (%d bytes)\n", PIX_BYTES);
    esp_restart();
  }
  memset(pix, 0, PIX_BYTES);
  FB = pix;  /* the menu draws its 320x120 bands into the same storage */
  printf("pix: %p (%s), doubles as menu FB\n", pix,
         ((uintptr_t)pix >> 24) == 0x3d ? "PSRAM" : "internal");
  vram = (uint8_t *)malloc(0x20000);  /* PSRAM; internal is taken by pix */

  osInit();
  delayMS(500);
#ifndef HEADLESS
  /* Clear the panel once; the picker is the boot screen now. The old blue/
   * green/magenta bring-up pattern lived here and is no longer needed -- the
   * pixel-readback self-test in lcdSelfTest() covers the same ground without
   * five seconds of flashing colours. */
  lcdFillScreen(0x0000);
#else
  printf("HEADLESS build: no LCD, no GPIO driven\n");
#endif
  printf("Hello world!\n");

  /* SD card: mount and inventory what is on it. Loading a ROM from here is the
   * next step and needs a decision about where it lives (see README) -- the
   * emulator addresses `rom` directly, so it must be either mmap'd flash or a
   * PSRAM copy, and an 8MB cart does not fit in 8MB of PSRAM alongside the
   * emulator's own ~600KB of buffers. */
  /* Boot into the picker, not straight into whatever is in flash.
   *
   * The emulator addresses `rom` directly through esp_partition_mmap, so the
   * chosen game must physically be in the flash partition. NVS records which
   * one is there, so re-picking the same game skips the copy and starts at once.
   */
  nvs_flash_init();
  /* Before the menu, not after it: the menu accepts a host ROM pick over this
   * same channel, and without the driver installed uart_read_bytes() fails. */
  osSerialInit();

  /* 320 MHz experiment: brief measured excursion every boot; stays there when
   * the one-shot hold flag (OS_CMD_CLK320) armed it. Before audio/SD init so
   * every peripheral gets configured under the clock it will run with. */
  bool at320 = clkProbeBoot();
  if (at320) {
    printf("CLK: running at 320 MHz\n");  /* lands on UART0; USB is dead */
  }

  menuInitInput();

  /* I2C first: the codec (0x18) and the touch controller (0x38) share one bus,
   * so a scan confirms both sets of schematic pins at once. */
  audioI2cInit();
  audioI2cScan();
  audioInit(47872);  /* matches the rate the emulator core actually produces */
  audioDumpRegs();
  /* User volume (settings menu, NVS "ui"/"vol"); 0 = mute. */
  {
    nvs_handle_t uih;
    uint8_t vol = AUDIO_DEFAULT_VOLUME_PCT;
    if (nvs_open("ui", NVS_READONLY, &uih) == ESP_OK) {
      nvs_get_u8(uih, "vol", &vol);
      nvs_close(uih);
    }
    audioSetVolume(vol);
  }

  /* Power state at boot: one line so a host (or a log reader) can sanity-check
   * the battery wiring and charge detection without the on-screen gauge. */
  osUsbPresent(); /* prime the SOF delta */
  delayMS(250);
  printf("POWER: battery=%dmV usb=%d\n", osBatteryMv(), osUsbPresent());

  static char flashed[260] = {0};
  bool haveFlashed = romGetFlashed(flashed, sizeof(flashed));
  /* Which game ends up running, for the save file name. */
  static char runningName[260] = {0};
  if (haveFlashed) {
    snprintf(runningName, sizeof(runningName), "%s", flashed);
  }
  bool sdOk = sdMount();

  if (sdOk) {
    sdEnsureSaveDir();
    static sdRomEntry roms[32];
    int nRoms = sdListRoms(roms, 32);
    printf("SD: %d rom(s) in %s\n", nRoms, SD_ROM_DIR);
    for (int i = 0; i < nRoms; i++) {
      printf("SD:   [%d] %-40s %.2f MB\n", i, roms[i].name,
             roms[i].size / 1048576.0);
    }

    if (nRoms > 0) {
      /* Retry loop: a failed copy returns to the picker rather than falling
       * through to the emulator against whatever stale garbage the flash
       * holds. Nothing starts without an explicit pick. */
      while (1) {
        int pick = menuChooseRom(roms, nRoms, haveFlashed ? flashed : NULL);
        if (pick < 0) {
          break;
        }
        printf("MENU: chose %s\n", roms[pick].name);
        snprintf(runningName, sizeof(runningName), "%s", roms[pick].name);
        if (haveFlashed && strcmp(flashed, roms[pick].name) == 0) {
          menuMessage("already in flash", "starting...");
          delayMS(600);
          break;
        }
        if (romCopyFromSd(roms[pick].name, roms[pick].size, roms[pick].code)) {
          break;
        }
        haveFlashed = false;   /* NVS record was invalidated by the copy */
        delayMS(2500);         /* leave the error on screen long enough to read */
      }
    } else if (haveFlashed) {
      menuMessage("no .gba in /sd/roms", "using flashed rom");
      delayMS(2500);
    } else {
      menuMessage("no .gba in /sd/roms", "add games, then reboot");
      delayMS(15000);
      esp_restart();
    }
  } else if (haveFlashed) {
    menuMessage("no sd card", "using flashed rom");
    delayMS(2500);
  } else {
    /* No card AND no recorded game: there is nothing legitimate to boot.
     * The old fallthrough flat-mapped whatever bytes the partition held --
     * on this board that was a Ruby image from an old partition layout,
     * which passed the header check and "ran" as a black screen. Park and
     * retry the card instead; a reseated card recovers by itself. */
    while (1) {
      menuMessage("no sd card", "insert card; retrying...");
      delayMS(5000);
      if (sdMount()) {
        esp_restart(); /* clean boot with the card present */
      }
    }
  }

  spi_flash_mmap_handle_t outHandle;
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "rom");
  if (partition == NULL) {
    ESP_LOGE(TAG, "Failed to find rom partition");
    return;
  }
  /* Sparse mmap: every all-0xFF page of the cart aliases ONE physical page.
   *
   * GBA carts are padded to a power of two and the padding is not only at the
   * end -- FireRed is 44.5% 0xFF with a 5.22MB hole in the middle. Skipping
   * those pages puts a 16MB cart into 9.31MB of flash while the emulator still
   * sees a flat, contiguous ROM. spi_flash_mmap_pages() takes the page list.
   */
  static int pages[512];
  uint32_t nPages = romGetPageMap(pages, 512);
  esp_err_t ret;
  if (nPages > 0) {
    /* Flash space is no longer the limit -- the MMU's data window is. Probe
     * downwards to find what this chip will actually give us, and report it,
     * rather than failing on a fixed guess. Trailing pages are blank padding,
     * so a shorter span still covers everything the game reads. */
    uint32_t want = nPages;
    ret = ESP_ERR_NO_MEM;
    /* floor of 64 pages assumed "any real cart"; GB carts are as small as
     * 2 pages, which made the loop body never run (mmap failed for every
     * .gb/.gbc). Probe down to a single page. */
    while (want >= 1) {
      ret = spi_flash_mmap_pages(pages, want, SPI_FLASH_MMAP_DATA,
                                 (const void **)&rom, &outHandle);
      if (ret == ESP_OK) {
        break;
      }
      want -= 4;
    }
    printf("rom: sparse map %u/%u pages (%.2f of %.2f MB virtual): %s\n",
           (unsigned)want, (unsigned)nPages, want / 16.0, nPages / 16.0,
           esp_err_to_name(ret));
    if (ret == ESP_OK && want < nPages) {
      /* The IDF mmap pool is MMU region 0 only (first 16MB of data VA), and
       * the app's own code+rodata entries live there too, so a full 256-page
       * cart can never fit through the driver. The cart's short mapping ends
       * right at region 1 (0x3D000000), which is empty below the PSRAM
       * window at 0x3D800000. Extend the mapping by hand with the ROM's MMU
       * setter -- virtually contiguous, the emulator still sees one flat
       * ROM. (FireRed JP keeps real data up to 15.9MB and crashed without
       * this: it read garbage where its own data belonged.) */
      uint32_t tailVa = (uint32_t)rom + want * 0x10000u;
      uint32_t tailN = nPages - want;
      printf("rom: mapped at %p; tail va 0x%08x..0x%08x (%u pages)\n",
             (void *)rom, (unsigned)tailVa,
             (unsigned)(tailVa + tailN * 0x10000u), (unsigned)tailN);
      /* The tail may begin on leftover free pool pages in region 0 (the -=4
       * probe can under-grant by up to 3) before crossing into region 1, so
       * the real safety check is per MMU entry: every slot we claim must be
       * invalid right now. Nothing later in a game boot allocates from the
       * pool, so pages the driver thinks are free stay ours. */
      bool tailFree = tailVa + tailN * 0x10000u <= 0x3D800000u;
      for (uint32_t i = 0; tailFree && i < tailN; i++) {
        uint32_t idx = (tailVa - 0x3C000000u) / 0x10000u + i;
        if (!(FLASH_MMU_TABLE[idx] & MMU_INVALID)) {
          printf("rom: tail MMU slot %u already in use -- not touching it\n",
                 (unsigned)idx);
          tailFree = false;
        }
      }
      if (tailFree) {
        int rc = 0;
        uint32_t autoload = Cache_Suspend_DCache();
        for (uint32_t i = 0; i < tailN && rc == 0; i++) {
          rc = Cache_Dbus_MMU_Set(MMU_ACCESS_FLASH, tailVa + i * 0x10000u,
                                  (uint32_t)pages[want + i] * 0x10000u, 64, 1,
                                  0);
        }
        Cache_Invalidate_Addr(tailVa, tailN * 0x10000u);
        Cache_Resume_DCache(autoload);
        if (rc == 0) {
          printf("rom: tail %u page(s) hand-mapped into MMU region 1 -- full "
                 "%.2f MB visible\n",
                 (unsigned)tailN, nPages / 16.0);
          want = nPages;
        } else {
          printf("rom: tail MMU set failed rc=%d -- falling through\n", rc);
        }
      }
    }
    if (ret == ESP_OK && want < nPages) {
      printf("rom: WARNING mapped short by %u pages -- reads above %.2f MB "
             "will fault\n",
             (unsigned)(nPages - want), want / 16.0);
      /* Blank-alias pages read as 0xFF anyway, so losing them is harmless.
       * Losing a REAL page means the game will read garbage where its own
       * data should be and jump through a bad pointer -- refuse loudly
       * instead of crash-looping (FireRed JP keeps data up to 15.9MB). */
      int blankPage = (int)(partition->address / 0x10000);
      for (uint32_t i = want; i < nPages; i++) {
        if (pages[i] != blankPage) {
          printf("rom: page %u holds real data but got no MMU slot -- "
                 "refusing to start\n", (unsigned)i);
          menuMessage("rom too big to map",
                      "MMU pages exhausted; see serial log");
          while (1) delayMS(1000);
        }
      }
    }
    /* Bound for the save-string scan (gbaover.cpp): reads past the mapped
     * span would fault. */
    extern uint32_t espgba_rom_size;
    espgba_rom_size = want * 0x10000u;
  } else {
    /* No page map recorded. Legitimate only for a pre-sparse flat flash,
     * which always has a name record; without one this partition's bytes
     * are leftovers from older layouts and must not be booted no matter
     * how plausible their header looks (learned the hard way: a stale
     * Ruby image passed every header check). */
    if (!haveFlashed) {
      printf("rom: no page map and no flashed record -- refusing stale flash\n");
      menuMessage("nothing in flash", "pick a game from the sd card");
      delayMS(15000);
      esp_restart();
    }
    uint32_t mapSize = romGetFlashedSize();
    if (mapSize == 0 || mapSize > partition->size) {
      mapSize = partition->size;
    }
    mapSize = (mapSize + 0xFFFF) & ~0xFFFFu;
    ret = esp_partition_mmap(partition, 0, mapSize, SPI_FLASH_MMAP_DATA,
                             (const void **)&rom, &outHandle);
    printf("rom: flat map of %u bytes: %s\n", (unsigned)mapSize,
           esp_err_to_name(ret));
    extern uint32_t espgba_rom_size;
    espgba_rom_size = mapSize;
  }
  if (ret != ESP_OK) {
    printf("rom: mmap failed: %s\n", esp_err_to_name(ret));
    menuMessage("rom mmap failed", esp_err_to_name(ret));
    while (1) delayMS(1000);
  }

#ifndef HEADLESS
  /* Wipe the whole panel before any game starts: the menu / copy progress
   * screen otherwise stays visible around the game window forever. */
  lcdFillScreen(0x0000);
#endif

  /* Game Boy / Game Boy Color: same flash copy, same mmap, different core. */
  {
    const char *ext = strrchr(runningName, '.');
    if (ext && (strcasecmp(ext, ".gb") == 0 || strcasecmp(ext, ".gbc") == 0)) {
      extern void gbRun(const uint8_t *romData, size_t romSize,
                        const char *name);
      gbRun((const uint8_t *)rom, romGetFlashedSize(), runningName);
      esp_restart(); /* leaving a GB session returns to the picker */
    }
  }

  /* Buffer placement.
   *
   * malloc() sends anything over CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL to PSRAM,
   * so upstream put vram, workRAM and pix all in external RAM. VRAM is the
   * hottest of them -- the PPU re-reads tile data for every scanline -- and at
   * 128KB it is the only large buffer that fits in the ~161KB of free internal
   * SRAM. Try internal first and fall back so a tight build still boots.
   *
   * workRAM (256KB) and pix (160KB) cannot fit alongside it and stay in PSRAM.
   */
  printf("internal heap: free=%u largest_block=%u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT));

  /* pix was allocated internal+DMA at the very top of app_main -- it is both
   * the renderer target and the SPI blit source now. */
  workRAM = (uint8_t *)malloc(0x40000);
  bios = (uint8_t *)malloc(0x4000);
  libretro_save_buf = (uint8_t *)malloc(0x20000 + 0x2000);
  printf(
      "internalRAM: %p, vram: %p, workRAM: %p, bios: %p, pix: %p\n, "
      "libretro_save_buf: %p\n",
      internalRAM, vram, workRAM, bios, pix, libretro_save_buf);
  // Diagnostics: prove the mmap'd ROM is actually readable through `rom`
  // before blaming the renderer. A GBA header has "fixed" byte 0x96 at 0xB2.
  printf("rom hdr: title='%.12s' code='%.4s' fixed=0x%02x\n", (const char *)(rom + 0xA0),
         (const char *)(rom + 0xAC), rom[0xB2]);

  /* Refuse to run a partition that does not contain a cartridge.
   * Byte 0xB2 is the GBA header's fixed 0x96. Without this the core happily
   * executes garbage and panics in a reboot loop, which looks like "the game
   * stopped booting" rather than "the flash is empty". */
  if (rom[0xB2] != 0x96) {
    printf("rom: no valid GBA header (0xB2=0x%02x) -- flash is empty or stale\n",
           rom[0xB2]);
    /* Whatever NVS says is flashed, it is not runnable -- clear the record
     * so the menu stops offering an instant resume into garbage. This is
     * what recovers automatically after a partition-layout change. */
    extern void romInvalidateFlashed(void);
    romInvalidateFlashed();
    menuMessage("no game in flash", "insert sd card / pick a game");
    /* Slow cycle on purpose: with a dead SD there is nothing to do but wait
     * for a reseat, and a 15s period keeps the console readable instead of
     * flickering through reboots. No flash writes happen on this path. */
    delayMS(15000);
    esp_restart();
  }
  printf("rom first bytes: %02x %02x %02x %02x\n", rom[0], rom[1], rom[2], rom[3]);

  /* Blank the whole panel before the game starts. The UI covers all 320x240 but
   * the game only ever blits its centred 240x160 window, so whatever the menu
   * left behind stays visible in the border -- the progress bar was still
   * showing through. */
#ifndef HEADLESS
  lcdFillScreen(0x0000);
#endif

  {
    extern void hotpcInit(void);
    hotpcInit(); /* PSRAM histogram; sampling is a no-op until this runs */
  }
  emuInit();
  /* (Xtensa perf counters tested here: dead on this silicon/IDF combo --
   * even forced PSRAM misses count zero. Removed.) */
  /* Restore this game's save from SD and arm the periodic flush. Must come
   * after emuInit(): CPU init wipes the save buffer to 0xFF. */
  if (sdOk && runningName[0]) {
    saveInit(runningName);
  }
  /* Per-game idle-loop skip PC. Priority: NVS override (set once over serial
   * after a hot-PC histogram run), else a one-time scan of the cart for the
   * gen-3 vblank busy-wait, byte-exact (thumb):
   *   ldrh r1,[r2,#0x1c]; adds r0,r3,#0; ands r0,r1; cmp r0,#0; beq .-8
   * which is 46.6%% of all execution when interpreted. The scan replaces the
   * old two-entry game-code table so any cart carrying this engine gets the
   * skip (verified: 0x080008c6 in Emerald J+U, 0x080008aa in FireRed and
   * LeafGreen J -- one match each, none anywhere else in those images).
   * Only a unique, halfword-aligned match arms; Ruby/Sapphire use an older
   * wait loop, scan clean, and correctly stay unarmed. */
  {
    extern uint32_t espgba_idle_pc;
    {
      extern uint32_t espgba_hle_pc, espgba_hle_pc2;
      nvs_handle_t uih;
      /* Ship defaults: native mixer on (pure win), debug overlay off
       * (Select+Up toggles it in-game). */
      uint8_t dbgOn = 0;
      hleWanted = 1;
      if (nvs_open("ui", NVS_READONLY, &uih) == ESP_OK) {
        nvs_get_u8(uih, "hle", &hleWanted);
        nvs_get_u8(uih, "dbg", &dbgOn);
        nvs_close(uih);
      }
      showFps = dbgOn;
      /* The mixer itself is hooked by scanning IWRAM once the game has
       * copied its m4a engine there -- see the frame loop. Nothing to do
       * at start beyond honouring the setting. */
      (void)espgba_hle_pc;
      (void)espgba_hle_pc2;
    }
    char key[5] = {0};
    memcpy(key, rom + 0xAC, 4);
    for (int i = 0; i < 4; i++) {
      if (key[i] < 0x21 || key[i] > 0x7E) key[i] = '_';
    }
    /* AOT-translated hot blocks (tools/aot/xlate.py): armed only when the
     * cart's window bytes hash-match the generation source. */
    {
      extern int espgba_aot_arm(const char *code);
      int aotN = espgba_aot_arm(key);
      if (aotN > 0) {
        printf("AOT: %d translated blocks armed for %s\n", aotN, key);
      } else if (aotN == -1) {
        printf("AOT: window hash mismatch for %s -- staying interpreted\n", key);
      }
    }
    uint32_t pc = 0;
    const char *src = "scan";
    {
      extern uint32_t espgba_rom_size;
      static const uint8_t kSpin[10] = {0x91, 0x8b, 0x18, 0x1c, 0x08,
                                        0x40, 0x00, 0x28, 0xfa, 0xd0};
      uint32_t limit = espgba_rom_size;
      if (limit > 0x1000000) limit = 0x1000000;
      int matches = 0;
      for (uint32_t off = 0; off + sizeof(kSpin) <= limit; off += 2) {
        if (rom[off] == kSpin[0] &&
            memcmp(rom + off, kSpin, sizeof(kSpin)) == 0) {
          if (matches++ == 0) {
            pc = 0x08000000u + off;
          }
        }
      }
      if (matches != 1) {
        pc = 0; /* none or ambiguous: run without the skip */
      }
    }
    nvs_handle_t h;
    if (nvs_open("idle", NVS_READONLY, &h) == ESP_OK) {
      uint32_t nvsPc = 0;
      if (nvs_get_u32(h, key, &nvsPc) == ESP_OK && nvsPc) {
        pc = nvsPc;
        src = "NVS";
      }
      nvs_close(h);
    }
    if (pc) {
      espgba_idle_pc = pc;
      printf("IDLE: skip PC 0x%08x (%s, game %s)\n", (unsigned)pc, src, key);
    }
  }

  /* Auto-overclock, only ever HERE: every driver is initialized under stock
   * 240 first, which is the order proven to work (boot-at-278 misconfigures
   * I2S and kills audio). Serial dies with the switch. */
  /* The auto-overclock no longer fires here: it engages ~10 seconds INTO
   * gameplay (see the frame loop). The delay is invisible to a player and
   * gives any host session a guaranteed serial window to disarm or issue
   * commands -- the instant-switch version lost that race repeatedly. */
  int prevTimeStamp = 0;
  TickType_t fpsTick = xTaskGetTickCount();
  uint32_t lastSamp = 0;
  int stepMode = 0;
  while (1) {
    int req = osPollSerial();

    /* Step-record mode.
     *
     * The UART needs ~0.9s to ship a 76800-byte frame, far longer than a frame
     * takes to emulate. If the emulator free-ran during transfers the recording
     * would have gaps and the audio would not line up with the video. So once
     * the host asks for a step, the emulator advances exactly one frame per
     * request and idles in between -- capture is slower than real time, but the
     * result is continuous and perfectly synced.
     */
    if (req & OS_REQ_STEP) {
      if (!stepMode) {
        stepMode = 1;
        SetFrameskip(0);  // record every frame, not every other one
        printf("step mode: frameskip off\n");
      }
    } else if (stepMode) {
      delayMS(2);  // idle until the host asks for the next frame
      continue;
    }

    if (req & OS_REQ_FSKIP) {
      int fs = osTakeFrameskip();
      if (fs >= 0) {
        SetFrameskip(fs);
        printf("frameskip set to %d\n", fs);
      }
    }

    if (req & OS_REQ_SAVE) {
      saveFlush();
    }

    if (req & OS_REQ_CLK320) {
      saveFlush();      /* don't lose progress across the restart */
      clkRequest320();  /* does not return */
    }

    if (req & OS_REQ_CLK_NOW) {
      saveFlush(); /* the switch can wedge; progress goes to SD first */
      clkHoldNow(); /* audioMatchRate folds in the PLL ratio each second */
    }

    if (req & (OS_REQ_CLK_AUTO_ON | OS_REQ_CLK_AUTO_OFF)) {
      clkAutoSet((req & OS_REQ_CLK_AUTO_ON) != 0);
    }

    if (req & (OS_REQ_HLE_ON | OS_REQ_HLE_OFF)) {
      extern uint32_t espgba_hle_pc, espgba_hle_pc2, espgba_hle_hits,
          espgba_hle_bails;
      /* OFF must also stop the discovery rescan or it re-arms in ~5s; ON
       * leaves arming to the scan so the address is this cart's, not a
       * remembered one. */
      hleWanted = (req & OS_REQ_HLE_ON) ? 1 : 0;
      if (!hleWanted) {
        espgba_hle_pc = 1;
        espgba_hle_pc2 = 1;
      }
      printf("HLE: native m4a mixer %s (hits=%u bails=%u)\n",
             (req & OS_REQ_HLE_ON) ? "ON" : "OFF",
             (unsigned)espgba_hle_hits, (unsigned)espgba_hle_bails);
    }

    if (req & OS_REQ_HOTPC) {
      extern void hotpcDump(int topN);
      hotpcDump(96);
    }

    if (req & OS_REQ_PEEK) {
      uint32_t pa, pl;
      osTakePeek(&pa, &pl);
      if (pl > 4096) pl = 4096;
      printf("PEEK 0x%08x %u\n", (unsigned)pa, (unsigned)pl);
      for (uint32_t o = 0; o < pl; o += 32) {
        printf("PK %08x ", (unsigned)(pa + o));
        for (uint32_t j = 0; j < 32 && o + j < pl; j++) {
          uint32_t a = pa + o + j;
          uint8_t b = 0xEE;
          extern uint8_t paletteRAM[0x400], oam[0x400];
          switch (a >> 24) {
            case 0x02: b = workRAM[a & 0x3FFFF]; break;
            case 0x03: b = internalRAM[a & 0x7FFF]; break;
            case 0x05: b = paletteRAM[a & 0x3FF]; break;
            case 0x06: b = vram[a & 0x1FFFF]; break;
            case 0x07: b = oam[a & 0x3FF]; break;
            case 0x08: case 0x09: b = rom[a & 0x1FFFFFF]; break;
            default: break;
          }
          printf("%02x", b);
        }
        printf("\n");
      }
      printf("PEEK end\n");
    }

    if (req & OS_REQ_IDLE) {
      extern uint32_t espgba_idle_pc;
      uint32_t pc = osTakeIdlePc();
      espgba_idle_pc = pc ? pc : 1; /* 1 = disabled, never matches */
      printf("IDLE: skip PC set to 0x%08x\n", (unsigned)pc);
      nvs_handle_t h;
      if (nvs_open("idle", NVS_READWRITE, &h) == ESP_OK) {
        char key[5] = {0};
        memcpy(key, rom + 0xAC, 4);
        for (int i = 0; i < 4; i++) {
          if (key[i] < 0x21 || key[i] > 0x7E) key[i] = '_';
        }
        if (pc) {
          nvs_set_u32(h, key, pc);
        } else {
          nvs_erase_key(h, key);
        }
        nvs_commit(h);
        nvs_close(h);
      }
    }

    /* Delayed auto-overclock: ~10s into the game (600 frames), so hosts
     * always get a serial window first. */
    {
      static bool ocDone = false;
      if (!ocDone && frameCount > 600) {
        ocDone = true;
        if (clkAutoGet()) {
          printf("CLK: auto overclock engaging (delayed)\n");
          clkHoldNow();
        }
      }
    }

    /* Native m4a mixer, armed by discovery instead of a per-game table: the
     * game copies its SoundMainRAM into IWRAM early on; scanning for its
     * entry signature finds it wherever this cart linked it, so any gen-3
     * era ROM gets the native mixer (body verified byte-identical across
     * Ruby/Sapphire/FireRed/LeafGreen/Emerald J+U). Rescan every ~5s while
     * unarmed -- also re-arms after the fire-time signature check disarms a
     * stale hook (game reloaded its engine elsewhere). */
    if (hleWanted) {
      extern uint32_t espgba_hle_pc, espgba_hle_pc2;
      extern void espgba_hle_scan(void);
      static uint32_t hleAnnounced = 0;
      if (espgba_hle_pc == 1 && (frameCount % 300) == 60) {
        espgba_hle_scan();
      }
      if (espgba_hle_pc != 1 && hleAnnounced != espgba_hle_pc) {
        hleAnnounced = espgba_hle_pc;
        printf("HLE: native m4a mixer armed at 0x%08x%s (scan)\n",
               (unsigned)espgba_hle_pc,
               espgba_hle_pc2 != 1 ? " (+second engine)" : "");
      }
    }

    joy = osReadKey();
    /* Buttons-only shoulder triggers: while SELECT is held, Left = L and
     * Right = R (the combo consumes Select and the direction). Select+Up
     * toggles the debug strip. Gen-3 uses L/R only for optional shortcuts,
     * so a combo is a fair trade for not needing two more switches. */
    if (joy & (1u << 2)) {
      uint32_t combo = 0;
      if (joy & (1u << 5)) combo |= 1u << 9; /* L */
      if (joy & (1u << 4)) combo |= 1u << 8; /* R */
      static bool upWas = false;
      bool upNow = (joy & (1u << 6)) != 0;
      if (upNow && !upWas) {
        showFps ^= 1;
        clkFlashGuard(); /* never write NVS while overclocked */
        nvs_handle_t uih;
        if (nvs_open("ui", NVS_READWRITE, &uih) == ESP_OK) {
          nvs_set_u8(uih, "dbg", (uint8_t)showFps);
          nvs_commit(uih);
          nvs_close(uih);
        }
        clkFlashRestore(); /* overclock comes back if it was on */
        if (!showFps) {
          memset(dbgStrip, 0, sizeof(dbgStrip));
          lcdBlitRegion((uint8_t *)dbgStrip, GBA_X_OFF, GBA_Y_OFF - 12, 240,
                        10);
        }
      }
      upWas = upNow;
      if (combo) {
        joy = (joy & ~((1u << 2) | (1u << 4) | (1u << 5) | (1u << 6))) | combo;
      }
    }
    UpdateJoypad();
    audioLen = 0;  // capture only this frame's audio
    emuRunFrame();
    saveTick();

    /* Kill m4a reverb: the mixer's reverb pass re-reads the whole PCM buffer
     * every frame (measured ~4%% of all execution, IWRAM blocks 0x1b40/0x1b80).
     * Zeroing SoundInfo->reverb (byte +5) each frame makes SoundMainRAM take
     * its standard zero-fill path instead -- the same path games without
     * reverb use, so semantics stay sound; the music just plays dry. Found
     * generically through SOUND_INFO_PTR (0x03007FF0) with the "Smsh" ident
     * check, so it works for any m4a game, not just Emerald. */
    {
      uint32_t sip;
      memcpy(&sip, internalRAM + 0x7FF0, 4);
      if ((sip >> 24) == 0x03) {
        uint32_t id;
        memcpy(&id, internalRAM + (sip & 0x7FFF), 4);
        if ((id | 1) == 0x68736D53u || (id | 1) == 0x68736D55u) {
          internalRAM[(sip + 5) & 0x7FFF] = 0;
        }
      }
    }

    /* Frame limiter -- a problem we EARNED: with the idle-loop skip, light
     * scenes emulate faster than a real GBA (intro measured at 93 emu fps),
     * so cap at 59.73. Rebase whenever we fall behind: no catch-up debt,
     * heavy scenes stay exactly as fast as they can be. The 100 Hz FreeRTOS
     * tick is too coarse for a 16.7 ms frame, so sleep whole ticks only for
     * big leads and spin the remainder on the XTAL-driven esp_timer. */
    if (!stepMode) {
      /* Pace on SAMPLES, not loop iterations: one emuRunFrame() can emulate
       * more than one GBA frame when the drop logic is active (measured:
       * per-call pacing still let the intro run at 93 fps). The core makes
       * exactly 47872 stereo pairs per emulated second -- that IS emulated
       * time. */
      static int64_t limitBaseUs;
      static uint32_t limitBaseSamp;
      int64_t now = esp_timer_get_time();
      uint32_t samp = audioSamples;
      int64_t target =
          limitBaseUs + (int64_t)(samp - limitBaseSamp) * 1000000 / 47872;
      if (limitBaseUs == 0 || now > target + 50000) {
        limitBaseUs = now; /* behind (or first frame): rebase, no debt */
        limitBaseSamp = samp;
      } else if (now < target) {
        while (target - esp_timer_get_time() > 12000) {
          vTaskDelay(1);
        }
        while (esp_timer_get_time() < target) {
        }
      }
    }

    if (req & OS_REQ_STEP) {
      // Video is optional per-step; audio always goes out so the recording
      // stays gap-free even when the host records video at a lower rate.
      if (req & OS_REQ_SHOT) {
        osSendFrameStrided(pix, 240, 160, PIX_STRIDE);
      }
      osSendAudio((const uint8_t *)audioBuf, audioLen * (int)sizeof(int16_t));
      continue;
    }
    if (req & OS_REQ_SHOT) {
      osSendFrameStrided(pix, 240, 160, PIX_STRIDE);
    }
    if (req & OS_REQ_PANELDUMP) {
      lcdPanelDump();
    }
    if (frameCount % 60 == 0) {
      /* Benchmark line.
       *
       * `emu` is the metric that matters: systemOnWriteDataToSoundBuffer fires
       * exactly once per emulated GBA frame, so audioCallbacks counts real
       * emulated frames regardless of frameskip. 59.72 = full speed.
       * `draw` is only how many of those reached the framebuffer.
       *
       * From reset with no input the attract mode is deterministic, so this is
       * directly comparable between builds at the same elapsed time.
       */
      TickType_t now = xTaskGetTickCount();
      int msPassed = (now - fpsTick) * portTICK_PERIOD_MS;
      if (msPassed <= 0) msPassed = 1;
      fpsTick = now;

      uint32_t nowSamp = audioSamples;
      uint32_t dSamp = nowSamp - lastSamp;
      lastSamp = nowSamp;

      // 47872 Hz / 59.7275 fps = 801.5 stereo pairs per emulated GBA frame.
      // emu is in centi-fps to keep one decimal without floating point.
      int emuCentiFps = (int)((uint64_t)dSamp * 100 * 1000 / 8015 / (uint32_t)msPassed);
      /* draw = frames that actually hit the panel. Skipped frames no longer
       * blit, so the loop rate (old metric) would overstate it. */
      static uint32_t lastBlit = 0;
      int fps = (int)((blitCount - lastBlit) * 1000 / (uint32_t)msPassed);
      lastBlit = blitCount;
      lastDrawFps = fps;
      lastEmuFps = emuCentiFps / 10;
      /* Continuous slow-motion audio: output rate follows emulation speed. */
      audioMatchRate(emuCentiFps, clkAt320() ? 557 : 480, 480);
#if !defined(HEADLESS) && defined(PANEL_ILI9341)
      // Verify the game is actually reaching the glass: sample the middle of
      // the GBA window straight out of panel RAM.
      lcdProbeRow(GBA_X_OFF + 100, GBA_Y_OFF + 80, 8);
      /* And that the panel still holds its config: a glitched controller
       * shows backlight white forever while the game runs on. Re-init it. */
      if (lcdPanelCheck()) {
        printf("LCD: panel was reinitialised; picture restored\n");
      }
#endif
      /* Cycle split from the core's PROF counters: percent of wall-clock CPU
       * time spent in instruction execution / PPU line render / APU tick.
       * The remainder is everything else (timers, DMA, blit sync, IDF). */
      extern uint32_t espgba_prof[7];
      extern uint32_t espgba_insns;
      extern uint32_t espgba_insns_arm;
      uint32_t insns = espgba_insns; espgba_insns = 0;
      uint32_t insnsA = espgba_insns_arm; espgba_insns_arm = 0;
      /* per-mode cycles per emulated instruction; slot0=thumb, slot2=arm */
      int cpiT = insns ? (int)((uint64_t)espgba_prof[0] / insns) : 0;
      int cpiA = insnsA ? (int)((uint64_t)espgba_prof[2] / insnsA) : 0;
      int armPct = (insns + insnsA)
                       ? (int)((uint64_t)insnsA * 100 / (insns + insnsA))
                       : 0;
      uint32_t cyclesPerPct = (uint32_t)((uint64_t)msPassed * 240000ull / 100);
      int pCpu = (int)((espgba_prof[0] + espgba_prof[2]) / cyclesPerPct);
      int pGfx = (int)(espgba_prof[1] / cyclesPerPct);
      int pApu = (int)(espgba_prof[2] / cyclesPerPct);
      int pSpr = (int)(espgba_prof[3] / cyclesPerPct);   /* core 1 */
      int pBg = (int)(espgba_prof[4] / cyclesPerPct);    /* core 1 */
      int pTile = (int)(espgba_prof[5] / cyclesPerPct);  /* core 1: BG passes */
      int pMix = (int)(espgba_prof[6] / cyclesPerPct);   /* core 1: merge */
      memset(espgba_prof, 0, sizeof(espgba_prof));

      {
        unsigned oe = lastEmuFps < 0 ? 0 : lastEmuFps > 99 ? 99 : lastEmuFps;
        unsigned od = lastDrawFps < 0 ? 0 : lastDrawFps > 99 ? 99 : lastDrawFps;
        snprintf(overlayText, sizeof(overlayText), "%u/%u c%u w%u a%u", oe, od,
                 (unsigned)(pCpu < 0 ? 0 : pCpu > 99 ? 99 : pCpu),
                 (unsigned)(pGfx < 0 ? 0 : pGfx > 99 ? 99 : pGfx),
                 (unsigned)(armPct < 0 ? 0 : armPct > 99 ? 99 : armPct));
      }
      {
        extern uint32_t espgba_hle_hits, espgba_hle_bails, espgba_aot_hits;
        printf("HLESTAT hits=%u bails=%u aot=%u\n", (unsigned)espgba_hle_hits,
               (unsigned)espgba_hle_bails, (unsigned)espgba_aot_hits);
      }
      /* One battery/USB read a second costs nothing. */
      batteryMv = osBatteryMv();
      usbPowered = osUsbPresent();
      if (showFps) {
        dbgStripDraw();
        lcdBlitRegion((uint8_t *)dbgStrip, GBA_X_OFF, GBA_Y_OFF - 12, 240, 10);
      }
      printf("BENCH t=%lus emu=%d.%d draw=%d cpu=%d%% wait=%d%% apu=%d%% "
             "w_spr=%d%% w_bg=%d%% w_tile=%d%% w_mix=%d%% cpiT=%d cpiA=%d arm=%d%% DISPCNT=%02x%02x "
             "BLD=%02x%02x,%02x bat=%dmV usb=%d keys=%03x\n",
             (unsigned long)(now * portTICK_PERIOD_MS / 1000), emuCentiFps / 10,
             emuCentiFps % 10, fps, pCpu, pGfx, pApu, pSpr, pBg, pTile, pMix, cpiT, cpiA, armPct,
             ioMem[1], ioMem[0], ioMem[0x51], ioMem[0x50], ioMem[0x54],
             batteryMv, usbPowered, (unsigned)(osReadKey() & 0x3FF));
    }
  }
}