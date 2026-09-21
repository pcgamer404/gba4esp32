#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stdint.h>

#define SD_MOUNT     "/sd"
#define SD_ROM_DIR  SD_MOUNT "/roms"
#define SD_ROM_GB_DIR  SD_ROM_DIR "/gb"
#define SD_ROM_GBC_DIR SD_ROM_DIR "/gbc"
#define SD_ROM_GBA_DIR SD_ROM_DIR "/gba"
#define SD_SAVE_DIR SD_MOUNT "/saves"

typedef struct {
  char name[256]; /* FAT long names are up to 255 chars */
  uint32_t size;
  char title[13]; /* GBA header 0xA0, 12 bytes */
  char code[5];   /* GBA header 0xAC, 4 bytes, e.g. AXVJ */
  bool fits;      /* false if larger than the rom flash partition */
} sdRomEntry;

#define SD_ART_DIR SD_MOUNT "/art"
#define SD_PACK_DIR SD_MOUNT "/packed"
#define ICON_W 48
#define ICON_H 48

bool sdMount(void);
int sdListRoms(sdRomEntry *out, int max);
bool sdEnsureSaveDir(void);

#ifdef __cplusplus
}
#endif
