#pragma once

#include "config.h"

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
int lcdPanelCheck(void); /* re-init a glitched panel; 1 = it was reset */
void lcdPanelDump(void); /* stream whole panel RAM in SHOT format */ /* read panel RAM back */            /* read back panel ID/MADCTL/pixel format */
void lcdFillScreen(uint16_t colour); /* full-panel fill, sized from LCD_W/LCD_H */
void delayMS(int ms);
void osInit();
uint32_t osReadKey();

/* USB serial control channel.
 *
 * The host can inject keys, select ROMs, request screenshots, change runtime
 * settings, and collect diagnostics. osPollSerial() is non-blocking and is
 * called from the emulation loop. Injected keys are OR'd into osReadKey(),
 * alongside the physical handheld buttons.
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
/* Battery voltage in mV. This handheld has no battery ADC, so returns -1. */
int osBatteryMv(void);
/* Non-zero while a USB host is connected (SOF frame counter ticking between
 * calls). Poll at 1Hz or slower. */
int osUsbPresent(void);
/* Set alongside OS_REQ_PEEK; the requested window. */
void osTakePeek(uint32_t *addr, uint32_t *len);

void osSerialInit(void);
/* Drop the USB serial backend before clock changes that invalidate the
 * USB-Serial-JTAG clock. */
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

/* Where the 240x160 GBA frame lands on the panel. */
#define GBA_X_OFF ((LCD_W - 240) / 2)
#define GBA_Y_OFF ((LCD_H - 160) / 2)

#ifdef __cplusplus
}
#endif