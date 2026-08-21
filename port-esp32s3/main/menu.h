#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sd.h"

#ifdef __cplusplus
extern "C" {
#endif

void menuInitInput(void);
bool touchCalLoad(void);
bool menuRunTouchCalibration(void);
void menuFlush(void);
void menuMessage(const char *line1, const char *line2);
void menuProgress(const char *label, int pct);

/* Blocks until buttons/touch/serial select an entry. Returns the index.
 * Nothing ever starts without an explicit pick. */
int menuChooseRom(const sdRomEntry *roms, int n, const char *flashed);

void menuOverlayFps(const char *text); /* into FB, after byteswap */

bool romGetFlashed(char *out, size_t len);
uint32_t romGetFlashedSize(void);
uint32_t romGetPageMap(int *pagesOut, uint32_t maxPages);
bool romCopyFromSd(const char *name, uint32_t size, const char *code);

#ifdef __cplusplus
}
#endif
