/* Hot-PC histogram: which GBA code actually burns the cycles. AOT-PLAN.md
 * milestone 1 -- the translation set must be chosen from measured execution,
 * not static guesswork.
 *
 * The interpreter samples every 64th executed instruction (both dispatch
 * sites in gba.cpp) into an open-addressed table keyed by 64-byte PC block
 * plus ARM/Thumb mode. 4096 entries cover a full game's hot code; overflow
 * is counted, not silently lost. Dump-and-reset over serial (OS_CMD_HOTPC),
 * so each dump is the interval since the previous one. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"

#define HOTPC_BITS 12
#define HOTPC_SIZE (1u << HOTPC_BITS)

typedef struct {
  uint32_t key; /* ((pc >> 6) << 1) | isArm; 0 = empty */
  uint32_t cnt;
} hotpcEntry;

/* PSRAM: 32KB of internal .bss would evict the pix DMA buffer (measured as
 * a FATAL-restart boot loop). One PSRAM access per 64 instructions is
 * noise. Allocated by hotpcInit(); sampling is a no-op until then. */
static hotpcEntry *tbl;
static uint32_t total, dropped;

void hotpcInit(void) {
  tbl = heap_caps_calloc(HOTPC_SIZE, sizeof(hotpcEntry), MALLOC_CAP_SPIRAM);
  if (tbl == NULL) {
    tbl = calloc(HOTPC_SIZE, sizeof(hotpcEntry));
  }
}

/* Incremented at both dispatch sites; record fires on wrap. Global so the
 * increment inlines into the interpreter loop. */
uint32_t espgba_hot_tick;

void IRAM_ATTR espgba_hotpc_record(uint32_t pc, uint32_t isArm) {
  if (tbl == NULL) {
    return;
  }
  uint32_t key = ((pc >> 6) << 1) | (isArm & 1u);
  uint32_t h = (key * 2654435761u) >> (32 - HOTPC_BITS);
  for (uint32_t probe = 0; probe < 8; probe++) {
    hotpcEntry *e = &tbl[(h + probe) & (HOTPC_SIZE - 1)];
    if (e->key == key) {
      e->cnt++;
      total++;
      return;
    }
    if (e->key == 0) {
      e->key = key;
      e->cnt = 1;
      total++;
      return;
    }
  }
  dropped++;
}

/* Print the top blocks, largest first, then reset. Output is line-oriented
 * for the host-side report tool: block base address, mode, sample count. */
void hotpcDump(int topN) {
  if (tbl == NULL) {
    printf("HOTPC not initialised\n");
    return;
  }
  printf("HOTPC total=%u dropped=%u\n", (unsigned)total, (unsigned)dropped);
  for (int rank = 0; rank < topN; rank++) {
    uint32_t best = 0, bestIdx = 0;
    for (uint32_t i = 0; i < HOTPC_SIZE; i++) {
      if (tbl[i].key && tbl[i].cnt > best) {
        best = tbl[i].cnt;
        bestIdx = i;
      }
    }
    if (best == 0) break;
    uint32_t key = tbl[bestIdx].key;
    printf("HOTPC 0x%08x %c %u\n", (unsigned)((key >> 1) << 6),
           (key & 1) ? 'A' : 'T', (unsigned)best);
    tbl[bestIdx].cnt = 0; /* consumed */
  }
  for (uint32_t i = 0; i < HOTPC_SIZE; i++) {
    tbl[i].key = 0;
    tbl[i].cnt = 0;
  }
  total = 0;
  dropped = 0;
  printf("HOTPC end\n");
}
