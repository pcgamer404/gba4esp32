/*
 * Per-game overrides (save type, flash size, RTC, mirroring), lifted verbatim
 * from libretro/libretro.cpp lines 226-424.
 *
 * The ESP32-S3 port compiles only gba/memory/sound/system from the core, so it
 * never got this table -- and upstream's emuInit() never called it. Without it
 * enableRtc stays false and flashSize stays 0, and Pokemon Gen 3 hangs during
 * its boot-time save/RTC probe with DISPCNT forced-blank (0x80): the emulator
 * runs at full speed but renders a black screen forever.
 *
 * Extracted rather than linking libretro.cpp, which drags in the whole libretro
 * frontend API. Kept byte-identical to upstream so it can be re-diffed.
 */
#include <stdio.h>
#include <string.h>

#include "../../components/gba/gba.h"
#include "../../components/gba/globals.h"
#include "../../components/gba/memory.h"
#include "../../components/gba/types.h"

typedef struct {
  char romtitle[256];
  char romid[5];
  int flashSize;
  int saveType;
  int rtcEnabled;
  int mirroringEnabled;
  int useBios;
} ini_t;

/* Force this table into flash. It is const, but its string-pointer members
 * make GCC emit it as .data.rel.ro, which IDF's linker script places in DRAM:
 * 72KB of internal RAM held hostage by a table read exactly once at boot.
 * An explicit .rodata.* section lands it in DROM instead. */
static const ini_t gbaover[256] __attribute__((section(".rodata.gbaover"))) = {
    // romtitle,							    	romid	flash	save	rtc	mirror
    // bios
    {"2 Games in 1 - Dragon Ball Z - The Legacy of Goku I & II (USA)", "BLFE",
     0, 1, 0, 0, 0},
    {"2 Games in 1 - Dragon Ball Z - Buu's Fury + Dragon Ball GT - "
     "Transformation (USA)",
     "BUFE", 0, 1, 0, 0, 0},
    {"Boktai - The Sun Is in Your Hand (Europe)(En,Fr,De,Es,It)", "U3IP", 0, 0,
     1, 0, 0},
    {"Boktai - The Sun Is in Your Hand (USA)", "U3IE", 0, 0, 1, 0, 0},
    {"Boktai 2 - Solar Boy Django (USA)", "U32E", 0, 0, 1, 0, 0},
    {"Boktai 2 - Solar Boy Django (Europe)(En,Fr,De,Es,It)", "U32P", 0, 0, 1, 0,
     0},
    {"Bokura no Taiyou - Taiyou Action RPG (Japan)", "U3IJ", 0, 0, 1, 0, 0},
    {"Card e-Reader+ (Japan)", "PSAJ", 131072, 0, 0, 0, 0},
    {"Classic NES Series - Bomberman (USA, Europe)", "FBME", 0, 1, 0, 1, 0},
    {"Classic NES Series - Castlevania (USA, Europe)", "FADE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Donkey Kong (USA, Europe)", "FDKE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Dr. Mario (USA, Europe)", "FDME", 0, 1, 0, 1, 0},
    {"Classic NES Series - Excitebike (USA, Europe)", "FEBE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Legend of Zelda (USA, Europe)", "FZLE", 0, 1, 0, 1,
     0},
    {"Classic NES Series - Ice Climber (USA, Europe)", "FICE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Metroid (USA, Europe)", "FMRE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Pac-Man (USA, Europe)", "FP7E", 0, 1, 0, 1, 0},
    {"Classic NES Series - Super Mario Bros. (USA, Europe)", "FSME", 0, 1, 0, 1,
     0},
    {"Classic NES Series - Xevious (USA, Europe)", "FXVE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Zelda II - The Adventure of Link (USA, Europe)",
     "FLBE", 0, 1, 0, 1, 0},
    {"Digi Communication 2 - Datou! Black Gemagema Dan (Japan)", "BDKJ", 0, 1,
     0, 0, 0},
    {"e-Reader (USA)", "PSAE", 131072, 0, 0, 0, 0},
    {"Dragon Ball GT - Transformation (USA)", "BT4E", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - Buu's Fury (USA)", "BG3E", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - Taiketsu (Europe)(En,Fr,De,Es,It)", "BDBP", 0, 1, 0, 0,
     0},
    {"Dragon Ball Z - Taiketsu (USA)", "BDBE", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku II International (Japan)", "ALFJ", 0,
     1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku II (Europe)(En,Fr,De,Es,It)", "ALFP",
     0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku II (USA)", "ALFE", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy Of Goku (Europe)(En,Fr,De,Es,It)", "ALGP", 0,
     1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku (USA)", "ALGE", 131072, 1, 0, 0, 0},
    {"F-Zero - Climax (Japan)", "BFTJ", 131072, 0, 0, 0, 0},
    {"Famicom Mini Vol. 01 - Super Mario Bros. (Japan)", "FMBJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 12 - Clu Clu Land (Japan)", "FCLJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 13 - Balloon Fight (Japan)", "FBFJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 14 - Wrecking Crew (Japan)", "FWCJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 15 - Dr. Mario (Japan)", "FDMJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 16 - Dig Dug (Japan)", "FTBJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 17 - Takahashi Meijin no Boukenjima (Japan)", "FTBJ", 0,
     1, 0, 1, 0},
    {"Famicom Mini Vol. 18 - Makaimura (Japan)", "FMKJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 19 - Twin Bee (Japan)", "FTWJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 20 - Ganbare Goemon! Karakuri Douchuu (Japan)", "FGGJ",
     0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 21 - Super Mario Bros. 2 (Japan)", "FM2J", 0, 1, 0, 1,
     0},
    {"Famicom Mini Vol. 22 - Nazo no Murasame Jou (Japan)", "FNMJ", 0, 1, 0, 1,
     0},
    {"Famicom Mini Vol. 23 - Metroid (Japan)", "FMRJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 24 - Hikari Shinwa - Palthena no Kagami (Japan)",
     "FPTJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 25 - The Legend of Zelda 2 - Link no Bouken (Japan)",
     "FLBJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 26 - Famicom Mukashi Banashi - Shin Onigashima - Zen "
     "Kou Hen (Japan)",
     "FFMJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 27 - Famicom Tantei Club - Kieta Koukeisha - Zen Kou "
     "Hen (Japan)",
     "FTKJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 28 - Famicom Tantei Club Part II - Ushiro ni Tatsu "
     "Shoujo - Zen Kou Hen (Japan)",
     "FTUJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 29 - Akumajou Dracula (Japan)", "FADJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 30 - SD Gundam World - Gachapon Senshi Scramble Wars "
     "(Japan)",
     "FSDJ", 0, 1, 0, 1, 0},
    {"Game Boy Wars Advance 1+2 (Japan)", "BGWJ", 131072, 0, 0, 0, 0},
    {"Golden Sun - The Lost Age (USA)", "AGFE", 65536, 0, 0, 1, 0},
    {"Golden Sun (USA)", "AGSE", 65536, 0, 0, 1, 0},
    {"Iridion II (Europe) (En,Fr,De)", "AI2P", 0, 5, 0, 0, 0},
    {"Iridion II (USA)", "AI2E", 0, 5, 0, 0, 0},
    {"Koro Koro Puzzle - Happy Panechu! (Japan)", "KHPJ", 0, 4, 0, 0, 0},
    {"Mario vs. Donkey Kong (Europe)", "BM5P", 0, 3, 0, 0, 0},
    {"Pocket Monsters - Emerald (Japan)", "BPEJ", 131072, 0, 1, 0, 0},
    {"Pocket Monsters - Fire Red (Japan)", "BPRJ", 131072, 0, 0, 0, 0},
    {"Pocket Monsters - Leaf Green (Japan)", "BPGJ", 131072, 0, 0, 0, 0},
    {"Pocket Monsters - Ruby (Japan)", "AXVJ", 131072, 0, 1, 0, 0},
    {"Pocket Monsters - Sapphire (Japan)", "AXPJ", 131072, 0, 1, 0, 0},
    {"Pokemon Mystery Dungeon - Red Rescue Team (USA, Australia)", "B24E",
     131072, 0, 0, 0, 0},
    {"Pokemon Mystery Dungeon - Red Rescue Team (En,Fr,De,Es,It)", "B24P",
     131072, 0, 0, 0, 0},
    {"Pokemon - Blattgruene Edition (Germany)", "BPGD", 131072, 0, 0, 0, 0},
    {"Pokemon - Edicion Rubi (Spain)", "AXVS", 131072, 0, 1, 0, 0},
    {"Pokemon - Edicion Esmeralda (Spain)", "BPES", 131072, 0, 1, 0, 0},
    {"Pokemon - Edicion Rojo Fuego (Spain)", "BPRS", 131072, 1, 0, 0, 0},
    {"Pokemon - Edicion Verde Hoja (Spain)", "BPGS", 131072, 1, 0, 0, 0},
    {"Pokemon - Eidicion Zafiro (Spain)", "AXPS", 131072, 0, 1, 0, 0},
    {"Pokemon - Emerald Version (USA, Europe)", "BPEE", 131072, 0, 1, 0, 0},
    {"Pokemon - Feuerrote Edition (Germany)", "BPRD", 131072, 0, 0, 0, 0},
    {"Pokemon - Fire Red Version (USA, Europe)", "BPRE", 131072, 0, 0, 0, 0},
    {"Pokemon - Leaf Green Version (USA, Europe)", "BPGE", 131072, 0, 0, 0, 0},
    {"Pokemon - Rubin Edition (Germany)", "AXVD", 131072, 0, 1, 0, 0},
    {"Pokemon - Ruby Version (USA, Europe)", "AXVE", 131072, 0, 1, 0, 0},
    {"Pokemon - Sapphire Version (USA, Europe)", "AXPE", 131072, 0, 1, 0, 0},
    {"Pokemon - Saphir Edition (Germany)", "AXPD", 131072, 0, 1, 0, 0},
    {"Pokemon - Smaragd Edition (Germany)", "BPED", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Emeraude (France)", "BPEF", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Rouge Feu (France)", "BPRF", 131072, 0, 0, 0, 0},
    {"Pokemon - Version Rubis (France)", "AXVF", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Saphir (France)", "AXPF", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Vert Feuille (France)", "BPGF", 131072, 0, 0, 0, 0},
    {"Pokemon - Versione Rubino (Italy)", "AXVI", 131072, 0, 1, 0, 0},
    {"Pokemon - Versione Rosso Fuoco (Italy)", "BPRI", 131072, 0, 0, 0, 0},
    {"Pokemon - Versione Smeraldo (Italy)", "BPEI", 131072, 0, 1, 0, 0},
    {"Pokemon - Versione Verde Foglia (Italy)", "BPGI", 131072, 0, 0, 0, 0},
    {"Pokemon - Versione Zaffiro (Italy)", "AXPI", 131072, 0, 1, 0, 0},
    {"Rockman EXE 4.5 - Real Operation (Japan)", "BR4J", 0, 0, 1, 0, 0},
    {"Rocky (Europe)(En,Fr,De,Es,It)", "AROP", 0, 1, 0, 0, 0},
    {"Rocky (USA)(En,Fr,De,Es,It)", "AR8e", 0, 1, 0, 0, 0},
    {"Sennen Kazoku (Japan)", "BKAJ", 131072, 0, 1, 0, 0},
    {"Shin Bokura no Taiyou - Gyakushuu no Sabata (Japan)", "U33J", 0, 1, 1, 0,
     0},
    {"Super Mario Advance 4 (Japan)", "AX4J", 131072, 0, 0, 0, 0},
    {"Super Mario Advance 4 - Super Mario Bros. 3 (Europe)(En,Fr,De,Es,It)",
     "AX4P", 131072, 0, 0, 0, 0},
    {"Super Mario Advance 4 - Super Mario Bros 3 - Super Mario Advance 4 v1.1 "
     "(USA)",
     "AX4E", 131072, 0, 0, 0, 0},
    {"Top Gun - Combat Zones (USA)(En,Fr,De,Es,It)", "A2YE", 0, 5, 0, 0, 0},
    {"Yoshi's Universal Gravitation (Europe)(En,Fr,De,Es,It)", "KYGP", 0, 4, 0,
     0, 0},
    {"Yoshi no Banyuuinryoku (Japan)", "KYGJ", 0, 4, 0, 0, 0},
    {"Yoshi - Topsy-Turvy (USA)", "KYGE", 0, 1, 0, 0, 0},
    {"Yu-Gi-Oh! GX - Duel Academy (USA)", "BYGE", 0, 2, 0, 0, 1},
    {"Yu-Gi-Oh! - Ultimate Masters - 2006 (Europe)(En,Jp,Fr,De,Es,It)", "BY6P",
     0, 2, 0, 0, 0},
    {"Zoku Bokura no Taiyou - Taiyou Shounen Django (Japan)", "U32J", 0, 0, 1,
     0, 0}};


/* Set by main.cpp after the cart mmap: bytes of `rom` safe to read. */
uint32_t espgba_rom_size = 0;

/* Find `pat` in the ROM (bounded memmem: the ROM is full of zero bytes, so
 * strstr cannot be used). */
static bool romHasString(const char *pat, uint32_t limit) {
  size_t n = strlen(pat);
  if (limit < n) return false;
  for (uint32_t i = 0; i + n <= limit; i++) {
    if (rom[i] == (uint8_t)pat[0] && memcmp(rom + i, pat, n) == 0)
      return true;
  }
  return false;
}

void load_image_preferences(void) {
  char buffer[5];
  buffer[0] = rom[0xac];
  buffer[1] = rom[0xad];
  buffer[2] = rom[0xae];
  buffer[3] = rom[0xaf];
  buffer[4] = 0;

  printf("GameID in ROM is: %s\n", buffer);

  bool found = false;
  int found_no = 0;

  for (int i = 0; i < 256; i++) {
    if (!strcmp(gbaover[i].romid, buffer)) {
      found = true;
      found_no = i;
      break;
    }
  }

  if (found) {
    enableRtc = gbaover[found_no].rtcEnabled;

    if (gbaover[found_no].flashSize != 0)
      flashSize = gbaover[found_no].flashSize;
    else
      flashSize = 65536;

    cpuSaveType = gbaover[found_no].saveType;

    mirroringEnable = gbaover[found_no].mirroringEnabled;
    printf("SAVE: override table hit for %s\n", buffer);
  } else {
    /* Not in the table: detect the save hardware from the ID strings the
     * standard save libraries embed in every commercial cart (and nearly
     * every romhack). Without this, a FLASH1M game missing from the table
     * keeps the 64KB default, its boot-time save probe never succeeds, and
     * it sits in forced blank forever -- a white screen at boot. Priority
     * matters: FLASH1M before the FLASH prefix it contains. */
    uint32_t limit = espgba_rom_size ? espgba_rom_size : (4 * 1024 * 1024);
    if (romHasString("FLASH1M_V", limit)) {
      flashSize = 0x20000;
      cpuSaveType = 0;
      printf("SAVE: detected FLASH1M (128KB flash)\n");
    } else if (romHasString("FLASH512_V", limit) ||
               romHasString("FLASH_V", limit)) {
      flashSize = 0x10000;
      cpuSaveType = 0;
      printf("SAVE: detected FLASH 64KB\n");
    } else if (romHasString("SRAM_V", limit) ||
               romHasString("SRAM_F_V", limit)) {
      cpuSaveType = 2;
      printf("SAVE: detected SRAM\n");
    } else if (romHasString("EEPROM_V", limit)) {
      cpuSaveType = 1;
      printf("SAVE: detected EEPROM\n");
    } else {
      printf("SAVE: no ID string found; keeping defaults\n");
    }
    if (romHasString("SIIRTC_V", limit)) {
      enableRtc = true;
      printf("SAVE: detected RTC (SIIRTC)\n");
    }
  }


    printf("RTC = %d.\n", enableRtc);
    printf( "flashSize = %d.\n", flashSize);
    printf( "cpuSaveType = %d.\n", cpuSaveType);
    printf( "mirroringEnable = %d.\n", mirroringEnable);
  
}
