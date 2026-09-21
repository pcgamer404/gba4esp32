#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load /sd/saves/<rom>.sav into libretro_save_buf (if present) and arm the
 * periodic dirty check. Call after emuInit(), before the first frame. */
void saveInit(const char *romName);

/* Call once per emulated frame; checksums the save buffer every few seconds
 * and flushes to SD once a change has settled. */
void saveTick(void);

/* Write the buffer out now (temp file + rename). Returns success. */
bool saveFlush(void);

#ifdef __cplusplus
}
#endif
