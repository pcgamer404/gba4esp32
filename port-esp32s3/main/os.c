
#include "os.h"
#include "clkprobe.h"
#include "esp_rom_sys.h"
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_vfs_dev.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

void delayMS(int ms) { vTaskDelay(ms / portTICK_PERIOD_MS); }

#define USE_HORIZONTAL 0
spi_device_handle_t spiDev0;

/* Transfer layer: deliberately simple, matching upstream's proven structure.
 *
 * I previously made this asynchronous (queued DMA, hardware CS) for a 24% fps
 * win, then chunked it, and each step broke the picture in a new way -- while
 * every check I had (build, benchmark, UART screenshots) stayed green, because
 * the framebuffer was always correct and only what reached the panel was wrong.
 *
 * So: back to blocking transfers with manual CS, exactly like upstream, with
 * one required change -- chunks must stay under the ESP32-S3's hard 32768-byte
 * per-transaction limit (SPI_LL_DATA_MAX_BIT_LEN = 1<<18 bits). The driver only
 * validates against max_transfer_sz, so an oversized transaction returns ESP_OK
 * and is silently truncated. Upstream's 28800 was already under it.
 *
 * Optimisation goes back in only after a picture is confirmed, one step at a
 * time, each verified on the panel.
 */
#define LCD_CHUNK_BYTES 28800 /* upstream's value; must stay < 32768 */

/* Asynchronous blit, re-introduced now that correctness is verifiable.
 *
 * The constraint that broke every earlier attempt: CS must stay LOW from the
 * 0x2C memory-write command through ALL of the pixel data. So instead of
 * letting the driver drive CS per transaction, CS is asserted manually before
 * the window commands and released only when the queued data has drained --
 * which happens at the START of the next frame, so the transfer overlaps the
 * emulation of that frame.
 *
 * Per frame:  wait+release CS -> byteswap -> CS low -> window -> queue chunks
 *
 * DC stays high for every queued chunk (they are all data), so no per-transfer
 * DC callback is needed.
 */
#define LCD_MAX_CHUNKS 6
static int fbTransPending = 0;
static spi_transaction_t fbTrans[LCD_MAX_CHUNKS];
static int fbCsHeld = 0;

static void lcdCsLow(void);
static void lcdCsHigh(void);

void lcdWaitFB(void) {
  while (fbTransPending > 0) {
    spi_transaction_t *done = NULL;
    ESP_ERROR_CHECK(spi_device_get_trans_result(spiDev0, &done, portMAX_DELAY));
    fbTransPending--;
  }
  if (fbCsHeld) {
    fbCsHeld = 0;
    lcdCsHigh();  // ends the memory write started before the queued chunks
  }
}

/* CS must stay LOW across a command and the data that belongs to it.
 *
 * Proven on this panel: lcdReadReg puts command+data in ONE transaction (CS
 * low throughout) and reads back exactly what was written. Everything that
 * split them across transactions -- 0x2C then pixels, 0x2E then pixels -- had
 * CS rise in between and the operation was discarded: writing red and reading
 * it back returned ffffff.
 *
 * Upstream raised CS after every transfer, which its ST7789 tolerated; this
 * ILI9341 does not. Nested so lcdSetWindow can be called inside a blit without
 * releasing CS.
 */
static int csDepth = 0;

static void lcdCsLow(void) {
  if (csDepth++ == 0) {
    gpio_set_level(PIN_LCD_CS, 0);
  }
}

static void lcdCsHigh(void) {
  if (--csDepth <= 0) {
    csDepth = 0;
    gpio_set_level(PIN_LCD_CS, 1);
  }
}

static void lcdWrite(uint8_t *buf, int len, int isCmd) {
  if (len <= 0) {
    return;
  }
  /* Drain any queued async blit FIRST.
   *
   * spi_device_transmit() is internally queue_trans + get_trans_result, so if
   * our own transactions are still outstanding it pops one of THOSE, then
   * uninstall_priv_desc() calls free() on a pointer from our static descriptor
   * that was never heap-allocated -> heap corruption -> StoreProhibited.
   * Found via addr2line on the panic backtrace; the trigger was lcdProbeRow()
   * issuing blocking SPI mid-blit.
   */
  lcdWaitFB();
  lcdCsLow();
  gpio_set_level(PIN_LCD_DC, isCmd ? 0 : 1);
  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = 8 * (size_t)len;
  t.tx_buffer = buf;
  ESP_ERROR_CHECK(spi_device_transmit(spiDev0, &t));
  gpio_set_level(PIN_LCD_DC, 1);
  lcdCsHigh();
}

static void lcdCmd8(uint8_t cmd) { lcdWrite(&cmd, 1, 1); }
static void lcdDat8(uint8_t dat) { lcdWrite(&dat, 1, 0); }
static void lcdDat16(uint16_t dat) {
  uint8_t buf[2] = {dat >> 8, dat & 0xff};
  lcdWrite(buf, 2, 0);
}

void lcdSetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  lcdCmd8(0x2a);
  lcdDat16(x1);
  lcdDat16(x2);
  lcdCmd8(0x2b);
  lcdDat16(y1);
  lcdDat16(y2);
  lcdCmd8(0x2c);
}

void lcdWriteFB(uint8_t *buf, int len) {
  while (len > 0) {
    int chunk = len > LCD_CHUNK_BYTES ? LCD_CHUNK_BYTES : len;
    lcdWrite(buf, chunk, 0);
    buf += chunk;
    len -= chunk;
  }
}

void lcdBlitRegion(uint8_t *buf, int x, int y, int w, int h) {
  /* Synchronous variant: CS held low across window + data, released here. */
  lcdCsLow();
  lcdSetWindow(x, y, x + w - 1, y + h - 1);
  lcdWriteFB(buf, w * h * 2);
  lcdCsHigh();
}

void lcdBlitRegionAsync(uint8_t *buf, int x, int y, int w, int h) {
  /* Queue the frame and return with CS still asserted. lcdWaitFB() drains the
   * queue and releases CS; the caller must invoke it before touching `buf`
   * again, which systemDrawScreen does at the top of the next frame. */
  lcdWaitFB();

  lcdCsLow();
  lcdSetWindow(x, y, x + w - 1, y + h - 1);
  gpio_set_level(PIN_LCD_DC, 1);  /* all queued chunks are data */

  int len = w * h * 2;
  int off = 0;
  int n = 0;
  while (off < len && n < LCD_MAX_CHUNKS) {
    int chunk = len - off;
    if (chunk > LCD_CHUNK_BYTES) {
      chunk = LCD_CHUNK_BYTES;
    }
    memset(&fbTrans[n], 0, sizeof(fbTrans[n]));
    fbTrans[n].length = 8 * (size_t)chunk;
    fbTrans[n].tx_buffer = buf + off;
    ESP_ERROR_CHECK(spi_device_queue_trans(spiDev0, &fbTrans[n], portMAX_DELAY));
    fbTransPending++;
    off += chunk;
    n++;
  }
  fbCsHeld = 1;  /* released by lcdWaitFB() once the queue drains */
}

#ifdef PANEL_ILI9341
/* Read a controller register.
 *
 * ILI9341 reads need one dummy byte after the command before real data, and
 * the read must happen in the same transaction as the command (CS held low),
 * so this uses a single full-duplex transfer rather than a command followed by
 * a separate read.
 */
/* Second handle purely for register reads.
 *
 * The ILI9341 accepts a 40MHz+ write clock but its *read* timing is limited to
 * roughly 6.6MHz; reading at the write clock returns zeros. Registers are read
 * rarely, so a separate slow device on the same bus is the simple fix.
 */
static spi_device_handle_t spiDevSlow;

static void lcdReadReg(uint8_t cmd, uint8_t *out, int len) {
  lcdWaitFB();
  uint8_t tx[8] = {cmd, 0, 0, 0, 0, 0, 0, 0};
  uint8_t rx[8] = {0};
  int total = len + 1;  // +1 dummy byte
  if (total > 8) total = 8;

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = 8 * total;
  t.rxlength = 8 * total;
  t.tx_buffer = tx;
  t.rx_buffer = rx;
  lcdCsLow();
  gpio_set_level(PIN_LCD_DC, 0);  /* command */
  ESP_ERROR_CHECK(spi_device_transmit(spiDevSlow, &t));
  gpio_set_level(PIN_LCD_DC, 1);
  lcdCsHigh();

  for (int i = 0; i < len; i++) {
    out[i] = rx[i + 1];  // skip the dummy
  }
}

/* Read pixels back out of the panel's own frame memory (RAMRD, 0x2E).
 *
 * The only way to check what the display actually received without a human
 * looking at it. ILI9341 returns each pixel as 3 bytes (R,G,B, 6 bits each,
 * left-aligned) after one dummy byte, regardless of COLMOD.
 */
static void lcdReadPixels(int x, int y, int n, uint8_t *rgb /* 3*n bytes */) {
  lcdCsLow();  /* keep CS low from the window commands through the payload */
  lcdSetWindow(x, y, x + n - 1, y);
  lcdCmd8(0x2E);

  int total = 1 + 3 * n;  /* dummy + payload */
  static uint8_t tx[64], rx[64];
  if (total > (int)sizeof(tx)) {
    total = (int)sizeof(tx);
  }
  memset(tx, 0, sizeof(tx));
  memset(rx, 0, sizeof(rx));

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = 8 * total;
  t.rxlength = 8 * total;
  t.tx_buffer = tx;
  t.rx_buffer = rx;

  gpio_set_level(PIN_LCD_DC, 1);  /* data phase of the read */
  ESP_ERROR_CHECK(spi_device_transmit(spiDevSlow, &t));
  lcdCsHigh();

  memcpy(rgb, rx + 1, 3 * n);
}

/* Stream the ENTIRE panel's frame memory back over serial in the SHOT wire
 * format (<FB len> + RGB565-BE). The read clock is ~6.6MHz so this takes a
 * couple of seconds -- diagnosis only. Answers "what is ON THE GLASS" without
 * a camera: every white-screen hunt tonight needed exactly this. */
void lcdPanelDump(void) {
  static uint8_t row[3 * 16];
  char hdr[24];
  int n = snprintf(hdr, sizeof(hdr), "\n<FB %d>\n", LCD_W * LCD_H * 2);
  extern void osSerialWriteRaw(const void *buf, int len);
  osSerialWriteRaw(hdr, n);
  for (int y = 0; y < LCD_H; y++) {
    for (int x = 0; x < LCD_W; x += 16) {
      lcdReadPixels(x, y, 16, row);
      uint8_t out[32];
      for (int i = 0; i < 16; i++) {
        /* panel returns 6-6-6 as 3 bytes; pack back to 565 big-endian */
        uint16_t v = ((row[3*i] >> 3) << 11) | ((row[3*i+1] >> 2) << 5)
                     | (row[3*i+2] >> 3);
        out[2*i] = v >> 8;
        out[2*i+1] = v & 0xFF;
      }
      osSerialWriteRaw(out, 32);
    }
  }
  osSerialWriteRaw("\n</FB>\n", 7);
}

/* Sample a row of the panel's frame memory and report how much of it is
 * non-black. Lets the running game be verified end to end -- emulator ->
 * framebuffer -> SPI -> panel RAM -- without anyone looking at the screen. */
void lcdProbeRow(int x, int y, int n) {
  uint8_t rgb[3 * 16];
  if (n > 16) n = 16;
  lcdReadPixels(x, y, n, rgb);
  int nonBlack = 0;
  for (int i = 0; i < n; i++) {
    if (rgb[3 * i] || rgb[3 * i + 1] || rgb[3 * i + 2]) nonBlack++;
  }
  ESP_LOGI("LCD", "panel probe @(%d,%d): %d/%d non-black, first px %02x%02x%02x",
           x, y, nonBlack, n, rgb[0], rgb[1], rgb[2]);
}

void lcdSelfTest(void) {
  uint8_t id[3] = {0}, madctl = 0, pixfmt = 0;

  /* 1. Are register reads trustworthy at all?
   *
   * I previously quoted MADCTL=0x28 as proof of landscape mode, but ID4 read
   * 00 00 00 and MADCTL read 0x00 on the very next boot. Write two DIFFERENT
   * values and see whether the readback tracks. If it does not, every read
   * below is noise and must not be used as evidence.
   */
  lcdCmd8(0x36); lcdDat8(0x28);
  lcdReadReg(0x0B, &madctl, 1);
  uint8_t back28 = madctl;
  lcdCmd8(0x36); lcdDat8(0x48);
  lcdReadReg(0x0B, &madctl, 1);
  uint8_t back48 = madctl;
  lcdCmd8(0x36); lcdDat8(0x28);  /* restore landscape */

  int readsWork = (back28 != back48);
  ESP_LOGI("LCD", "read check: wrote 28 -> %02x, wrote 48 -> %02x  => reads %s",
           back28, back48, readsWork ? "TRUSTWORTHY" : "UNRELIABLE (ignore)");

  lcdReadReg(0xD3, id, 3);
  lcdReadReg(0x0C, &pixfmt, 1);
  ESP_LOGI("LCD", "ID4=%02x %02x %02x (ILI9341=00 93 41, ST7796=00 77 96) COLMOD=%02x",
           id[0], id[1], id[2], pixfmt);

  /* 2. Does a write actually land in frame memory?
   *
   * Paint 8 pixels red through the real blit path, then read those same pixels
   * back. Tests window addressing, DC/CS framing and byte order end to end,
   * with no human in the loop.
   */
  static uint16_t probe[8];
  for (int i = 0; i < 8; i++) {
    probe[i] = 0x00F8;  /* big-endian 0xF800 = red */
  }
  lcdBlitRegion((uint8_t *)probe, 0, 0, 8, 1);

  uint8_t rgb[24] = {0};
  lcdReadPixels(0, 0, 8, rgb);
  ESP_LOGI("LCD", "pixel probe: wrote RED x8, read back %02x%02x%02x %02x%02x%02x "
                  "%02x%02x%02x (expect ~f8 00 00)",
           rgb[0], rgb[1], rgb[2], rgb[3], rgb[4], rgb[5], rgb[6], rgb[7], rgb[8]);

    ESP_LOGI("LCD", "geometry: LCD_W=%d LCD_H=%d  GBA window x=%d..%d y=%d..%d",
           LCD_W, LCD_H, GBA_X_OFF, GBA_X_OFF + 239, GBA_Y_OFF, GBA_Y_OFF + 159);
}

/* Fill the whole panel with one colour, sized from LCD_W/LCD_H rather than
 * from the framebuffer, so a wrong assumption about panel size shows up as a
 * partially-filled screen instead of silently leaving old content behind. */
void lcdFillScreen(uint16_t colour) {
  static uint16_t line[LCD_W];
  for (int i = 0; i < LCD_W; i++) {
    line[i] = (uint16_t)((colour >> 8) | (colour << 8));  // panel wants big-endian
  }
  for (int y = 0; y < LCD_H; y++) {
    lcdBlitRegion((uint8_t *)line, 0, y, LCD_W, 1);
  }
}

/* ILI9341 power-on sequence.
 *
 * Several of these registers are undocumented in the public datasheet but are
 * what every working ILI9341 driver sends; omitting them gives a blank or
 * washed-out panel. Values follow Adafruit / TFT_eSPI.
 */
static void lcdInitILI9341(void) {
  lcdCmd8(0x01);  // SWRESET
  delayMS(150);

  static const uint8_t seq[] = {
      /* cmd, len, data... */
      0xEF, 3, 0x03, 0x80, 0x02,              // undocumented, required
      0xCF, 3, 0x00, 0xC1, 0x30,              // power control B
      0xED, 4, 0x64, 0x03, 0x12, 0x81,        // power on sequence
      0xE8, 3, 0x85, 0x00, 0x78,              // driver timing A
      0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,  // power control A
      0xF7, 1, 0x20,                          // pump ratio
      0xEA, 2, 0x00, 0x00,                    // driver timing B
      0xC0, 1, 0x23,                          // power control 1 (VRH)
      0xC1, 1, 0x10,                          // power control 2
      0xC5, 2, 0x3E, 0x28,                    // VCOM control 1
      0xC7, 1, 0x86,                          // VCOM control 2
      // MADCTL MV|BGR: landscape 320x240, panel is BGR-ordered
      // (vendor setup sets TFT_RGB_ORDER TFT_BGR).
      0x36, 1, 0x28,
      0x3A, 1, 0x55,              // 16 bits/pixel
      0xB1, 2, 0x00, 0x18,        // frame rate
      0xB6, 3, 0x08, 0x82, 0x27,  // display function control
      0xF2, 1, 0x00,              // 3Gamma off
      0x26, 1, 0x01,              // gamma curve 1
      0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07,
      0x10, 0x03, 0x0E, 0x09, 0x00,  // positive gamma
      0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08,
      0x0F, 0x0C, 0x31, 0x36, 0x0F,  // negative gamma
  };

  for (size_t i = 0; i < sizeof(seq);) {
    uint8_t cmd = seq[i++];
    uint8_t len = seq[i++];
    lcdCmd8(cmd);
    for (uint8_t j = 0; j < len; j++) {
      lcdDat8(seq[i + j]);
    }
    i += len;
  }

  lcdCmd8(0x21);  // INVON -- vendor setup sets TFT_INVERSION_ON
  lcdCmd8(0x11);  // SLPOUT
  delayMS(120);
  lcdCmd8(0x29);  // DISPON
}
#endif

void lcdInit() {
#if PIN_SYS_RSTN >= 0
  gpio_set_direction(PIN_SYS_RSTN, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_LCD_CS, 1);
  gpio_set_level(PIN_SYS_RSTN, 0);
  delayMS(500);
  gpio_set_level(PIN_SYS_RSTN, 1);
#endif
  gpio_set_direction(PIN_LCD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_LCD_DC, 1);
  gpio_set_direction(PIN_LCD_DC, GPIO_MODE_OUTPUT);
  static const spi_bus_config_t buscfg = {
      .miso_io_num = PIN_SPI0_MISO,
      .mosi_io_num = PIN_SPI0_MOSI,
      .sclk_io_num = PIN_SPI0_SCLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 120000,
  };
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
  ESP_LOGI("HAL", "SPI2 initialized");
  // Hardware CS + a pre-transfer callback for DC, so asynchronous transfers are
  // framed correctly without the CPU toggling pins around them.
  static const spi_device_interface_config_t devcfg = {
      .clock_speed_hz = LCD_SPI_HZ,
      .mode = 0,
      .spics_io_num = -1,   /* manual CS, as upstream */
      .queue_size = 7};
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spiDev0));

#ifdef PANEL_ILI9341
  // Slow companion handle for register reads (see lcdReadReg).
  static const spi_device_interface_config_t devcfg_slow = {
      .clock_speed_hz = 4000000,
      .mode = 0,
      .spics_io_num = -1,
      .queue_size = 1};
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg_slow, &spiDevSlow));
#endif

  ESP_LOGI("HAL", "Device Added");

#ifdef PANEL_ILI9341
  lcdInitILI9341();
  lcdSelfTest();
  if (PIN_LCD_BL >= 0) {
    // Backlight last: nothing is shown until the panel is initialised, which
    // avoids a flash of garbage RAM at power-on.
    gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_BL, 1);
    ESP_LOGI("HAL", "ILI9341 initialised, backlight on (GPIO %d)", PIN_LCD_BL);
  }
  return;
#endif

  //************* Start Initial Sequence **********//
  lcdCmd8(0x11);  // Sleep out
  delayMS(120);   // Delay 120ms
  //************* Start Initial Sequence **********//
  lcdCmd8(0x36);
  if (USE_HORIZONTAL == 0)
    lcdDat8(0x00);
  else if (USE_HORIZONTAL == 1)
    lcdDat8(0xC0);
  else if (USE_HORIZONTAL == 2)
    lcdDat8(0x70);
  else
    lcdDat8(0xA0);

  lcdCmd8(0x3A);
  lcdDat8(0x05);

  lcdCmd8(0xB2);
  lcdDat8(0x0C);
  lcdDat8(0x0C);
  lcdDat8(0x00);
  lcdDat8(0x33);
  lcdDat8(0x33);

  lcdCmd8(0xB7);
  lcdDat8(0x35);

  lcdCmd8(0xBB);
  lcdDat8(0x19);

  lcdCmd8(0xC0);
  lcdDat8(0x2C);

  lcdCmd8(0xC2);
  lcdDat8(0x01);

  lcdCmd8(0xC3);
  lcdDat8(0x12);

  lcdCmd8(0xC4);
  lcdDat8(0x20);

  lcdCmd8(0xC6);
  lcdDat8(0x0F);

  lcdCmd8(0xD0);
  lcdDat8(0xA4);
  lcdDat8(0xA1);

  lcdCmd8(0xE0);
  lcdDat8(0xD0);
  lcdDat8(0x04);
  lcdDat8(0x0D);
  lcdDat8(0x11);
  lcdDat8(0x13);
  lcdDat8(0x2B);
  lcdDat8(0x3F);
  lcdDat8(0x54);
  lcdDat8(0x4C);
  lcdDat8(0x18);
  lcdDat8(0x0D);
  lcdDat8(0x0B);
  lcdDat8(0x1F);
  lcdDat8(0x23);

  lcdCmd8(0xE1);
  lcdDat8(0xD0);
  lcdDat8(0x04);
  lcdDat8(0x0C);
  lcdDat8(0x11);
  lcdDat8(0x13);
  lcdDat8(0x2C);
  lcdDat8(0x3F);
  lcdDat8(0x44);
  lcdDat8(0x51);
  lcdDat8(0x2F);
  lcdDat8(0x1F);
  lcdDat8(0x1F);
  lcdDat8(0x20);
  lcdDat8(0x23);
  lcdCmd8(0x21);

  lcdCmd8(0x29);
}



// "a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "turbo", "menu"
int osKeyMap[12] = {PIN_KEY_A, PIN_KEY_B, PIN_KEY_SELECT, PIN_KEY_START, PIN_KEY_RIGHT, PIN_KEY_LEFT, PIN_KEY_UP, PIN_KEY_DOWN, -1, -1, -1, -1};

#if defined(BOARD_FNK0104AB)
/* 2x4 button matrix on the expansion headers (this board has exactly six
 * free GPIOs, all on the P2/P3 headers; ten discrete buttons need a matrix).
 * Rows are open-drain outputs driven low one at a time; pulled-up columns
 * read pressed = 0.
 *   row IO2: Up(IO14)   Down(IO21)  Left(IO43)   Right(IO44)
 *   row IO3: A (IO14)   B (IO21)    Select(IO43) Start(IO44)
 * Without per-button diodes a diagonal D-pad pair plus an action key sharing
 * one of their columns ghosts a fourth key. Gen-3 Pokemon never moves
 * diagonally, so this is acceptable; series 1N4148s remove it entirely. */
#define MATRIX_ROWS 2
#define MATRIX_COLS 4
static const gpio_num_t matrixRow[MATRIX_ROWS] = {GPIO_NUM_2, GPIO_NUM_3};
static const gpio_num_t matrixCol[MATRIX_COLS] = {GPIO_NUM_14, GPIO_NUM_21,
                                                  GPIO_NUM_43, GPIO_NUM_44};
static const uint8_t matrixBit[MATRIX_ROWS][MATRIX_COLS] = {
    {6, 7, 5, 4}, /* Up Down Left Right (GBA key bit numbers) */
    {0, 1, 2, 3}, /* A B Select Start */
};
static void matrixInit(void) {
  for (int r = 0; r < MATRIX_ROWS; r++) {
    gpio_reset_pin(matrixRow[r]);
    gpio_set_direction(matrixRow[r], GPIO_MODE_OUTPUT_OD);
    gpio_set_level(matrixRow[r], 1);
  }
  for (int c = 0; c < MATRIX_COLS; c++) {
    gpio_reset_pin(matrixCol[c]);
    gpio_set_direction(matrixCol[c], GPIO_MODE_INPUT);
    gpio_set_pull_mode(matrixCol[c], GPIO_PULLUP_ONLY);
  }
}
static uint32_t matrixScan(void) {
  uint32_t k = 0;
  for (int r = 0; r < MATRIX_ROWS; r++) {
    gpio_set_level(matrixRow[r], 0);
    esp_rom_delay_us(3);
    for (int c = 0; c < MATRIX_COLS; c++) {
      if (gpio_get_level(matrixCol[c]) == 0) {
        k |= 1u << matrixBit[r][c];
      }
    }
    gpio_set_level(matrixRow[r], 1);
  }
  return k;
}
#endif

static volatile uint32_t serialKeys = 0;

/* Battery voltage: GPIO9 = ADC1_CH8 behind a 1:2 divider (schematic R14/R15),
 * so the pack voltage is twice the pin reading. Shared by the menu gauge and
 * the in-game debug strip. Uses the chip's factory eFuse ADC calibration when
 * present (real millivolts); the raw formula is the ~+-10% fallback. Averages
 * 8 samples -- the speaker amp shares the rail and single reads jitter. */
#include "driver/adc.h"
#include "esp_adc_cal.h"
int osBatteryMv(void) {
  static bool adcInited = false;
  static bool haveCal = false;
  static esp_adc_cal_characteristics_t cal;
  if (!adcInited) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_8, ADC_ATTEN_DB_11);
    haveCal = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11,
                                       ADC_WIDTH_BIT_12, 0,
                                       &cal) != ESP_ADC_CAL_VAL_DEFAULT_VREF;
    adcInited = true;
  }
  int acc = 0;
  for (int i = 0; i < 8; i++) {
    int raw = adc1_get_raw(ADC1_CHANNEL_8);
    if (raw < 0) {
      return -1;
    }
    acc += raw;
  }
  int raw = acc / 8;
  int pinMv = haveCal ? (int)esp_adc_cal_raw_to_voltage(raw, &cal)
                      : raw * 3100 / 4095;
  return pinMv * 2;
}

/* USB power detect: a connected host sends SOF every 1ms, ticking the
 * USB-Serial-JTAG frame counter. The delta needs real time between reads, so
 * the check runs at most every 200ms and calls in between get the cached
 * answer -- back-to-back calls (menu draws the gauge twice per frame) would
 * otherwise see a frozen counter and flicker "no host". */
#include "soc/usb_serial_jtag_reg.h"
#include "esp_timer.h"
int osUsbPresent(void) {
  static uint32_t lastFrame = 0xFFFFFFFF;
  static int present = 0;
  static int64_t lastCheckUs = 0;
  int64_t now = esp_timer_get_time();
  if (lastCheckUs != 0 && now - lastCheckUs < 200000) {
    return present;
  }
  uint32_t cur = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
  if (lastFrame != 0xFFFFFFFF) {
    present = (cur != lastFrame);
  }
  lastFrame = cur;
  lastCheckUs = now;
  return present;
}
static volatile int pendingFrameskip = -1;
static volatile int serialPick = -1;
static volatile uint32_t serialIdlePc = 0;
static volatile uint32_t peekAddr = 0, peekLen = 0;

uint32_t osTakeIdlePc(void) { return serialIdlePc; }

void osTakePeek(uint32_t *addr, uint32_t *len) {
  *addr = peekAddr;
  *len = peekLen;
}

int osTakeSerialPick(void) {
  int p = serialPick;
  serialPick = -1;
  return p;
}

int osTakeFrameskip(void) {
  int v = pendingFrameskip;
  pendingFrameskip = -1;
  return v;
}

uint32_t osReadKey() {
  uint32_t ret = 0;
  for (int i = 0; i < 12; i++) {
    if (osKeyMap[i] != -1) {
      if (gpio_get_level(osKeyMap[i]) == 0) {
        ret |= 1 << i;
      }
    }
  }
#if defined(BOARD_FNK0104AB)
  ret |= matrixScan();
#endif
  return ret | serialKeys;
}

/* Serial backend.
 *
 * Two board variants exist: ones with a real USB-UART bridge on UART0 (the
 * CH340 boards the notes were written on) and ones exposing only the S3's
 * native USB-Serial-JTAG -- this Freenove, where UART0 TX43/RX44 go to no
 * connector at all. stdout reaches the host either way (the JTAG console is
 * configured as secondary), but serial INPUT and the binary frame/audio dumps
 * only ever touched UART0 -- so on this board every host command (pick,
 * frameskip, keys) silently vanished and every screenshot went to two unwired
 * pins. Install both peripherals, poll both for commands, and send dumps to
 * the USB one when it is up.
 */
static bool usbSerialUp;

void osSerialInit(void) {
#ifdef HEADLESS
  // Leave stdout on the default console. The control channel is unavailable.
  return;
#endif
  /* UART0 is GONE from this build: the console is USB-Serial-JTAG (sdkconfig)
   * and GPIO 43/44 now serve as button-matrix columns. */

  usb_serial_jtag_driver_config_t ucfg = {
      .tx_buffer_size = 8192,
      .rx_buffer_size = 8192,
  };
  usbSerialUp = usb_serial_jtag_driver_install(&ucfg) == ESP_OK;
  printf("SERIAL: UART0 + usb_serial_jtag (%s)\n",
         usbSerialUp ? "up" : "install failed");
}

/* One byte from whichever link the host is on. `ticks` only applies to the
 * USB path -- the UART read stays non-blocking. */
void osSerialMarkDead(void) { usbSerialUp = false; }

static int serialRead1(uint8_t *b, TickType_t ticks) {
  if (usbSerialUp && usb_serial_jtag_read_bytes(b, 1, ticks) == 1) {
    return 1;
  }
  return 0;
}

void osSerialWriteRaw(const void *buf, int len);

static void serialWrite(const void *buf, int len) {
  if (usbSerialUp) {
    usb_serial_jtag_write_bytes((const char *)buf, len, pdMS_TO_TICKS(2000));
  } /* else: 278-hold, output dropped -- UART0 pins belong to the buttons */
}

void osSerialWriteRaw(const void *buf, int len) { serialWrite(buf, len); }

int osPollSerial(void) {
  uint8_t b;
  int req = 0;
#ifdef HEADLESS
  return 0;  // no UART0 driver installed
#endif

  // Drain whatever is buffered; never block, this runs inside the frame loop.
  while (serialRead1(&b, 0) == 1) {
    if (b != OS_SERIAL_MAGIC) {
      continue;  // resync: ignore stray bytes
    }
    uint8_t cmd;
    if (serialRead1(&cmd, pdMS_TO_TICKS(20)) != 1) {
      continue;
    }
    if (cmd == OS_CMD_KEYS) {
      uint8_t lo, hi;
      if (serialRead1(&lo, pdMS_TO_TICKS(20)) != 1) continue;
      if (serialRead1(&hi, pdMS_TO_TICKS(20)) != 1) continue;
      serialKeys = ((uint32_t)hi << 8) | lo;
    } else if (cmd == OS_CMD_SHOT) {
      req |= OS_REQ_SHOT;
    } else if (cmd == OS_CMD_FSKIP) {
      uint8_t v = 0;
      if (serialRead1(&v, pdMS_TO_TICKS(20)) != 1) continue;
      pendingFrameskip = v;
      req |= OS_REQ_FSKIP;
    } else if (cmd == OS_CMD_PICK) {
      uint8_t v = 0;
      if (serialRead1(&v, pdMS_TO_TICKS(20)) != 1) continue;
      serialPick = v;
    } else if (cmd == OS_CMD_SAVE) {
      req |= OS_REQ_SAVE;
    } else if (cmd == OS_CMD_CLK320) {
      req |= OS_REQ_CLK320;
    } else if (cmd == OS_CMD_CLK_NOW) {
      req |= OS_REQ_CLK_NOW;
    } else if (cmd == OS_CMD_HOTPC) {
      req |= OS_REQ_HOTPC;
    } else if (cmd == OS_CMD_IDLE) {
      uint8_t p[4];
      int ok = 1;
      for (int i = 0; i < 4; i++) {
        if (serialRead1(&p[i], pdMS_TO_TICKS(20)) != 1) { ok = 0; break; }
      }
      if (!ok) continue;
      serialIdlePc = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      req |= OS_REQ_IDLE;
    } else if (cmd == OS_CMD_PEEK) {
      uint8_t p[6];
      int ok = 1;
      for (int i = 0; i < 6; i++) {
        if (serialRead1(&p[i], pdMS_TO_TICKS(20)) != 1) { ok = 0; break; }
      }
      if (!ok) continue;
      peekAddr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      peekLen = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
      req |= OS_REQ_PEEK;
    } else if (cmd == 0x0E) { /* probe PC: +4 bytes LE */
      uint8_t p4[4];
      int ok = 1;
      for (int i = 0; i < 4; i++) {
        if (serialRead1(&p4[i], pdMS_TO_TICKS(20)) != 1) { ok = 0; break; }
      }
      if (!ok) continue;
      extern uint32_t espgba_probe_pc, espgba_probe_hits;
      espgba_probe_pc = (uint32_t)p4[0] | ((uint32_t)p4[1] << 8) |
                        ((uint32_t)p4[2] << 16) | ((uint32_t)p4[3] << 24);
      espgba_probe_hits = 0;
      printf("PROBE: pc=0x%08x hits reset\n", (unsigned)espgba_probe_pc);
    } else if (cmd == 0x0F) { /* probe report */
      extern uint32_t espgba_probe_pc, espgba_probe_hits;
      printf("PROBE: pc=0x%08x hits=%u\n", (unsigned)espgba_probe_pc,
             (unsigned)espgba_probe_hits);
    } else if (cmd == 0x12) { /* dump the PANEL's own frame memory */
      req |= 0x2000; /* OS_REQ_PANELDUMP */
    } else if (cmd == 0x11) { /* ls: list /sd/roms and /sd/art */
      const char *dirs[2] = {"/sd/roms", "/sd/art"};
      for (int di = 0; di < 2; di++) {
        DIR *d = opendir(dirs[di]);
        printf("LS %s%s\n", dirs[di], d ? "" : " (missing)");
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
          printf("LS   %s\n", e->d_name);
        }
        closedir(d);
      }
      printf("LS end\n");
    } else if (cmd == 0x10) { /* put file: [1B pathlen][path][4B size] + data */
      uint8_t plen = 0;
      if (serialRead1(&plen, pdMS_TO_TICKS(200)) != 1 || plen == 0 ||
          plen > 120) continue;
      char path[160];
      snprintf(path, sizeof(path), "/sd/");
      int hdrok = 1;
      for (int i = 0; i < plen; i++) {
        uint8_t c;
        if (serialRead1(&c, pdMS_TO_TICKS(200)) != 1) { hdrok = 0; break; }
        path[4 + i] = (char)c;
      }
      if (!hdrok) continue;
      path[4 + plen] = 0;
      uint8_t sz[4];
      for (int i = 0; i < 4; i++) {
        if (serialRead1(&sz[i], pdMS_TO_TICKS(200)) != 1) { hdrok = 0; break; }
      }
      if (!hdrok) continue;
      uint32_t fsize = (uint32_t)sz[0] | ((uint32_t)sz[1] << 8) |
                       ((uint32_t)sz[2] << 16) | ((uint32_t)sz[3] << 24);
      /* Sized for full carts: the host repairs damaged SD ROMs over USB. */
      if (fsize > 32 * 1024 * 1024) continue;
      /* the art dir may not exist yet on this card */
      mkdir("/sd/art", 0775);
      FILE *pf = fopen(path, "wb");
      uint32_t got = 0;
      uint32_t acked = 0;
      uint8_t fbuf[256];
      while (got < fsize) {
        uint32_t want = fsize - got;
        if (want > sizeof(fbuf)) want = sizeof(fbuf);
        uint32_t n = 0;
        while (n < want) {
          if (serialRead1(&fbuf[n], pdMS_TO_TICKS(800)) != 1) break;
          n++;
        }
        if (n == 0) break;
        if (pf) fwrite(fbuf, 1, n, pf);
        got += n;
        /* Flow control: the sender stops at each 6KB window edge until this
         * ack -- an fwrite stall (FAT allocation on big cards) can no longer
         * overflow the 8KB USB rx buffer and silently drop bytes. */
        if (got - acked >= 6144 || got == fsize) {
          printf("PUTACK %u\n", (unsigned)got);
          acked = got;
        }
      }
      if (pf) fclose(pf);
      printf("PUTFILE %s %u/%u %s\n", path, (unsigned)got, (unsigned)fsize,
             (pf && got == fsize) ? "OK" : "FAIL");
    } else if (cmd == OS_CMD_HLE) {
      uint8_t v = 0;
      if (serialRead1(&v, pdMS_TO_TICKS(20)) != 1) continue;
      /* applied HERE, not via req bits: the menu also polls this parser and
       * used to eat these commands without acting on them */
      extern uint32_t espgba_hle_pc, espgba_hle_pc2;
      espgba_hle_pc = v ? 0x03001b50 : 1;
      espgba_hle_pc2 = v ? 0x03002918 : 1;
      printf("HLE: native m4a mixer %s (both engines)\n", v ? "ON" : "OFF");
    } else if (cmd == OS_CMD_CLK_AUTO) {
      uint8_t v = 0;
      if (serialRead1(&v, pdMS_TO_TICKS(20)) != 1) continue;
      clkAutoSet(v != 0); /* applied here: survives menu-context polling */
    } else if (cmd == OS_CMD_STEP) {
      // One parameter byte: non-zero means also ship the framebuffer. Audio is
      // always returned, so the host can record video at a lower rate than the
      // emulated frame rate without tearing a hole in the soundtrack.
      uint8_t withVideo = 0;
      if (serialRead1(&withVideo, pdMS_TO_TICKS(20)) != 1) {
        continue;
      }
      req |= OS_REQ_STEP;
      if (withVideo) req |= OS_REQ_SHOT;
    }
  }
  return req;
}

void osSendFrame(const uint8_t *fb, int len) {
  /* Framed so the host can find the payload amid log chatter. Header and
   * payload MUST use the same channel: printf goes through the buffered vfs
   * console while serialWrite hits the driver directly, and the header used
   * to arrive out of order (host saw pixels with no <FB> marker). */
  char hdr[24];
  int n = snprintf(hdr, sizeof(hdr), "\n<FB %d>\n", len);
  serialWrite(hdr, n);
  serialWrite(fb, len);
  serialWrite("\n</FB>\n", 7);
}

void osSendFrameStrided(const uint16_t *px, int w, int h, int stridePx) {
  /* Same wire format as osSendFrame (contiguous w*h RGB565-BE), sourced from
   * a wider buffer -- pix keeps vba-next's 256px stride but the host wants
   * the visible 240. Header via the same raw channel as the data. */
  char hdr[24];
  int n = snprintf(hdr, sizeof(hdr), "\n<FB %d>\n", w * h * 2);
  serialWrite(hdr, n);
  for (int y = 0; y < h; y++) {
    serialWrite(px + y * stridePx, w * 2);
  }
  serialWrite("\n</FB>\n", 7);
}

void osSendAudio(const uint8_t *pcm, int len) {
  // Signed 16-bit stereo interleaved at the rate passed to soundSetSampleRate().
  printf("\n<AU %d>\n", len);
  fflush(stdout);
  if (len > 0) {
    serialWrite(pcm, len);
  }
  printf("\n</AU>\n");
  fflush(stdout);
}



void osInit() {
#ifdef HEADLESS
  // Drive nothing: unknown board, unknown pinout. Input still works over the
  // serial control channel, which is all the host tooling needs.
  return;
#else
  lcdInit();
#if defined(BOARD_FNK0104AB)
  matrixInit();
#endif
  for (int i = 0; i < 12; i++) {
    int pin = osKeyMap[i];
    if (pin != -1) {
      // Set the GPIO as a input, pullup
      gpio_set_direction(pin, GPIO_MODE_INPUT);
      gpio_pullup_en(pin);
      // disable pulldown
      gpio_pulldown_dis(pin);
    }
  }
#endif
}