#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
void lcdInit();
void lcdSetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void lcdWriteFB(uint8_t *buf, int len);
void lcdWaitFB(void); /* reclaim the queued blit before reusing the buffer */
void lcdBlitRegion(uint8_t *buf, int x, int y, int w, int h);      /* blocking */
void lcdBlitRegionAsync(uint8_t *buf, int x, int y, int w, int h); /* queued; CS released by lcdWaitFB */
void lcdSelfTest(void);
void lcdProbeRow(int x, int y, int n);
void lcdPanelDump(void); /* stream whole panel RAM in SHOT format */ /* read panel RAM back */            /* read back panel ID/MADCTL/pixel format */
void lcdFillScreen(uint16_t colour); /* full-panel fill, sized from LCD_W/LCD_H */
void delayMS(int ms);
void osInit();
uint32_t osReadKey();

/* Serial control channel.
 *
 * There is no panel or button hardware on this board yet, so the host drives
 * input and reads frames over UART0 instead. osPollSerial() is non-blocking and
 * is called once per emulated frame; it returns non-zero when the host has
 * asked for a framebuffer dump. Injected keys are OR'd into osReadKey(), so
 * real GPIO buttons keep working unchanged once they are wired.
 */
#define OS_SERIAL_MAGIC 0xA5
#define OS_CMD_KEYS 0x01 /* + 2 bytes little-endian key bitmask */
#define OS_CMD_SHOT 0x02 /* request one framebuffer dump */
#define OS_CMD_STEP 0x03 /* advance exactly one frame, return video + audio */
#define OS_CMD_FSKIP 0x04 /* + 1 byte: set frameskip at runtime */
#define OS_CMD_PICK 0x05  /* + 1 byte: pick this ROM index in the menu and start */
#define OS_CMD_SAVE 0x06  /* flush the save buffer to SD now */
#define OS_CMD_CLK320 0x07 /* reboot into a one-shot 320MHz hold (clkprobe.h) */
#define OS_CMD_CLK_NOW 0x08 /* live-switch to 278MHz, no reboot (clkprobe.h) */
#define OS_CMD_CLK_AUTO 0x09 /* + 1 byte: 0/1 = auto-278-on-game-start flag */
#define OS_CMD_HOTPC 0x0A /* dump + reset the hot-PC histogram (hotpc.c) */
#define OS_CMD_IDLE 0x0B /* + 4 bytes LE: idle-loop PC to skip (0 = clear) */
#define OS_CMD_PEEK 0x0C /* + 4 bytes LE addr + 2 bytes LE len: hex dump of
                          * emulated memory (IWRAM/EWRAM/ROM) over serial */
#define OS_CMD_PANELDUMP 0x12 /* stream the panel's frame memory back (slow) */
#define OS_CMD_HLE 0x0D /* + 1 byte: 0/1 native m4a mixer (gba.cpp HLE) */

/* osPollSerial() return bits */
#define OS_REQ_SHOT 0x01
#define OS_REQ_STEP 0x02
#define OS_REQ_FSKIP 0x04
#define OS_REQ_SAVE 0x08
#define OS_REQ_CLK320 0x10
#define OS_REQ_CLK_NOW 0x20
#define OS_REQ_CLK_AUTO_ON 0x40
#define OS_REQ_CLK_AUTO_OFF 0x80
#define OS_REQ_HOTPC 0x100
#define OS_REQ_IDLE 0x200
#define OS_REQ_PEEK 0x400
#define OS_REQ_HLE_ON 0x800
#define OS_REQ_HLE_OFF 0x1000
#define OS_REQ_PANELDUMP 0x2000
/* Set alongside OS_REQ_IDLE; the PC that arrived with the command. */
uint32_t osTakeIdlePc(void);
/* Battery pack voltage in mV (ADC1_CH8 behind a 1:2 divider), -1 if the ADC
 * read failed. */
int osBatteryMv(void);
/* Non-zero while a USB host is connected (SOF frame counter ticking between
 * calls). Poll at 1Hz or slower. */
int osUsbPresent(void);
/* Set alongside OS_REQ_PEEK; the requested window. */
void osTakePeek(uint32_t *addr, uint32_t *len);

void osSerialInit(void);
/* Drop the USB backend: writes fall back to UART0 (unconnected pins, drains
 * harmlessly) and reads stop polling USB. For the 320MHz hold, where the
 * USB-Serial-JTAG's 48MHz clock no longer exists. */
void osSerialMarkDead(void);
int osPollSerial(void);
void osSendFrame(const uint8_t *fb, int len);
/* Same wire format, sourced from a wider buffer (pix is 256px-stride). */
void osSendFrameStrided(const uint16_t *px, int w, int h, int stridePx);
void osSendAudio(const uint8_t *pcm, int len);
int osTakeFrameskip(void); /* -1 if unchanged since last call */
/* Menu ROM index requested over serial, or -1. Lets bench.py start a specific
 * game from reset; the menu is otherwise buttons-only, which no host script
 * can drive. */
int osTakeSerialPick(void);

/* ---------------------------------------------------------------------------
 * Panel / board selection. Build with -DBOARD_FNK0104AB for the Freenove
 * FNK0104A/B (2.8" ILI9341). Default is upstream's bare ST7789 240x240 wiring.
 * ------------------------------------------------------------------------- */
#if defined(BOARD_FNK0104AB)

/* Freenove FNK0104AB, 2.8" ILI9341 240x320.
 *
 * Pins are taken from the vendor's own TFT_eSPI setup header
 * (Libraries/FNK0104AB/TFT_eSPI_Setups_v1.3.zip ->
 * FNK0104AB_2.8_240x320_ILI9341.h), not guessed.
 *
 * Driven rotated to 320x240 landscape with the 240x160 GBA frame centred,
 * giving a 40px border all round.
 *
 * Pins that MUST NOT be touched on this board -- audio codec and SD card:
 *   ES8311 I2S : MCK 17, BCK 18, DIN 16, DOUT 15, WS 21, amp enable 1
 *   ES8311 I2C : SCL 39, SDA 38
 *   SDMMC      : CLK 5, CMD 4, D0 6, D1 7, D2 2, D3 3
 * Upstream's PIN_SYS_RSTN was 21 -- the codec's word-select line -- and the
 * default button map below lands on the SD and I2S pins. Both are why this
 * board needs its own block rather than the generic one.
 */
#define PANEL_ILI9341 1
#define LCD_W (320)
#define LCD_H (240)

#define PIN_LCD_DC 46
#define PIN_LCD_CS 10
#define PIN_SPI0_MOSI 11
#define PIN_SPI0_MISO 13
#define PIN_SPI0_SCLK 12
#define PIN_SYS_RSTN (-1) /* RST tied to the board reset line */
#define PIN_LCD_BL 45     /* backlight, HIGH = on */
/* 55MHz nominal: the 260MHz overclock overdrives the SPI clock +8.3%, and at
 * a 60MHz base that lands past what this ILI9341 accepts -- the panel stops
 * taking writes ~10s into gameplay (white screen) while the game runs on.
 * 55 nominal = 59.6 under overclock, inside the proven envelope. */
#define LCD_SPI_HZ (55000000)

/* SD pins: VERIFIED BY MOUNTING, not from the vendor sketch.
 *
 * Freenove's Sketch_06.1_SDMMC_Test lists 4/5/6/7/2/3 for the FNK0104AB branch
 * and 40/38/39/41/48/47 for N/S. On this board only the second group works --
 * it mounts a card 4-bit and lists files, while the first group never gets a
 * response to ACMD41 and leaves D0 (GPIO 6) held low. Board revision or a doc
 * error; the hardware is the authority.
 */
#define SD_PIN_CMD 40
#define SD_PIN_CLK 38
#define SD_PIN_D0 39
#define SD_PIN_D1 41
#define SD_PIN_D2 48
#define SD_PIN_D3 47

/* No discrete buttons on this board; -1 disables each. Using the generic map
 * here would put "buttons" on the SD card and I2S lines. */
#define PIN_KEY_UP (-1)
#define PIN_KEY_DOWN (-1)
#define PIN_KEY_LEFT (-1)
#define PIN_KEY_RIGHT (-1)
#define PIN_KEY_A (-1)
#define PIN_KEY_B (-1)
#define PIN_KEY_SELECT (-1)
#define PIN_KEY_START (-1)

#else

#define LCD_W (240)
#define LCD_H (240)

#define PIN_LCD_DC 10
#define PIN_LCD_CS 11
#define PIN_SPI0_MOSI 13
#define PIN_SPI0_MISO 14
#define PIN_SPI0_SCLK 12
#define PIN_SYS_RSTN 21
#define PIN_LCD_BL (-1)
#define LCD_SPI_HZ (60000000)

#define SD_PIN_CMD 4
#define SD_PIN_CLK 5
#define SD_PIN_D0 6
#define SD_PIN_D1 7
#define SD_PIN_D2 2
#define SD_PIN_D3 3

/* Buttons. Remapped off upstream's pins, which used GPIO 0, 45 and 46 --
 * all ESP32-S3 strapping pins. Holding those at reset changes boot mode
 * (GPIO 0 low = USB download mode), so a player pressing Up at power-on
 * would stop the console booting. See WIRING.md.
 *
 * Every pin below is safe on a bare devkit: not strapping (0/3/45/46), not
 * flash/octal-PSRAM (26-37), not console UART (43/44), not native USB (19/20).
 * All use internal pull-ups, so each button just shorts its pin to GND.
 */
#define PIN_KEY_UP (4)
#define PIN_KEY_DOWN (5)
#define PIN_KEY_LEFT (6)
#define PIN_KEY_RIGHT (7)
#define PIN_KEY_A (15)
#define PIN_KEY_B (16)
#define PIN_KEY_SELECT (17)
#define PIN_KEY_START (18)

#endif

/* Where the 240x160 GBA frame lands on the panel. */
#define GBA_X_OFF ((LCD_W - 240) / 2)
#define GBA_Y_OFF ((LCD_H - 160) / 2)

#ifdef __cplusplus
}
#endif