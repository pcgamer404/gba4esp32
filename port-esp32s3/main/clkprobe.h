#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 320 MHz experiment. The S3's clock tree has an undocumented divide-by-one
 * slot: with the BBPLL trained to 320 MHz (SYSTEM_PLL_FREQ_SEL=0) the
 * CPUPERIOD_SEL=2 encoding that normally means "480/2 = 240" becomes
 * "320/1 = 320". The 80 MHz branch is 320/4, so flash, PSRAM and APB timing
 * are untouched. Casualties: every 480-derived clock -- USB-Serial-JTAG
 * (console) and WiFi/BT. */

/* Measured CPU frequency in MHz, ccount against the XTAL-driven systimer. */
uint32_t clkMeasureMhz(void);

/* Boot-time probe: brief 320 excursion + integrity check, then revert and
 * print the numbers. If the one-shot hold flag is armed (clkRequest320) and
 * the excursion measured clean, STAYS at 320 -- USB serial is dead until the
 * next reset, fps proof is the LCD overlay. Returns true if now at 320. */
bool clkProbeBoot(void);

/* Arm the one-shot hold flag and restart. */
void clkRequest320(void);

/* Switch to the proven 278 MHz point RIGHT NOW, no reboot: drivers keep their
 * 240-era configs and their physical clocks scale +16%. USB serial dies.
 * Returns true if held (reverts automatically if the excursion fails). */
bool clkHoldNow(void);

bool clkAt320(void);

/* Drop back to stock 240 if currently overclocked. MUST be called before any
 * flash or NVS write: the overdriven PLL also overdrives the SPI flash clock
 * and writes at that speed have corrupted the cart image. No-op at stock. */
void clkFlashGuard(void);

/* Persistent auto-switch: hold 278 right after a game starts (drivers all
 * initialized at stock 240 first -- the proven order). Toggled over serial. */
bool clkAutoGet(void);
void clkAutoSet(bool on);

#ifdef __cplusplus
}
#endif
