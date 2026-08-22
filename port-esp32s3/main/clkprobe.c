/* 320 MHz overclock probe. See clkprobe.h for the theory.
 *
 * The entire excursion happens inside one flash-guard window (caches off,
 * other CPU stalled, interrupts off): switch to PLL320/div1, measure ccount
 * against the raw systimer (XTAL-driven, 16 MHz, immune to PLL surgery),
 * switch back to PLL480/240, and only then re-enable the caches. The caches
 * never run under the 320 PLL unless a hold was armed AND the measurement
 * came back sane -- the risky part (MSPI taps under a retrained PLL) is
 * opt-in, the measurement is not.
 *
 * Everything inside the window is IRAM: rtc_clk and regi2c_ctrl are `noflash`
 * in esp_hw_support/linker.lf, the helpers here are IRAM_ATTR, and the ROM
 * provides ets_update_cpu_frequency/esp_rom_delay_us. */
#include "clkprobe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp32s3/rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/system_reg.h"
#include "soc/systimer_reg.h"

#include "os.h"

/* IRAM symbols with no public prototype (see linker.lf note above). */
extern void rtc_clk_bbpll_configure(int xtal_freq_mhz, int pll_freq_mhz);
extern void regi2c_ctrl_write_reg(uint8_t block, uint8_t host_id,
                                  uint8_t reg_add, uint8_t data);
extern void regi2c_ctrl_write_reg_mask(uint8_t block, uint8_t host_id,
                                       uint8_t reg_add, uint8_t msb,
                                       uint8_t lsb, uint8_t data);
extern void spi_flash_disable_interrupts_caches_and_other_cpu(void);
extern void spi_flash_enable_interrupts_caches_and_other_cpu(void);
/* Per-chip PVT-calibrated regulator codes the IDF applies for 240 MHz. */
extern uint32_t g_dig_dbias_pvt_240m;
extern uint32_t g_rtc_dbias_pvt_240m;

/* regi2c_dig_reg.h is a private header; the four constants we need. */
#define DIG_REG_BLOCK 0x6D
#define DIG_REG_HOST 1
#define DIG_REG_EXT_RTC_DREG 4 /* bits 4:0 */
#define DIG_REG_EXT_DIG_DREG 6 /* bits 4:0 */
/* regi2c_bbpll.h: the BBPLL's feedback divider register. PLL out =
 * XTAL * (div7_0 + 4): the stock 480 config is div7_0=8; higher multipliers
 * overdrive the VCO past its rated point -- that is the experiment. The bias
 * registers (DR1/DR3/DCUR/DCHGP/VCO_DBIAS) are identical between the stock
 * 320 and 480 configs, so they are left as the 480 calibration set them. */
#define BBPLL_BLOCK 0x66
#define BBPLL_HOST 1
#define BBPLL_OC_DIV_7_0 3

/* One extra regulator step above the chip's own 240 MHz code. The field is
 * 5 bits; the PVT code is typically in the high 20s, so +2 is a mild bump,
 * not a max-out. Thermal margin is the user's heatsink problem. */
#define DBIAS_EXTRA_STEPS 2

static bool s_at320;

/* Breadcrumbs that survive a crash at 320 so the next (240) boot can report
 * what happened. */
#define CLK_MAGIC 0x0C320C32
#define CLK_PROBE_MAGIC 0x0BEEF320
static RTC_NOINIT_ATTR uint32_t s_holdMagic;
static RTC_NOINIT_ATTR uint32_t s_holdMhz;
/* Set right before an excursion, cleared right after it survives. If it is
 * still set at boot, the previous excursion killed the chip -- skip probing
 * so a firmware that can't do 320 still boots at 240 every time. */
static RTC_NOINIT_ATTR uint32_t s_probeArmed;
/* Where inside the excursion the previous attempt died, and what it measured
 * (stage 7+). Written from IRAM with caches off; RTC slow RAM is always
 * writable. */
static RTC_NOINIT_ATTR uint32_t s_stage;
static RTC_NOINIT_ATTR uint32_t s_stageMhz;
/* Candidate sweep state: which candidate to try next and what each measured
 * (RES_WEDGED for ones that took the chip down). Valid while s_candMagic
 * matches the current table. */
static RTC_NOINIT_ATTR uint32_t s_candMagic;
static RTC_NOINIT_ATTR uint32_t s_candIdx;
static RTC_NOINIT_ATTR uint32_t s_candRes[8];

static inline uint32_t ccount(void) {
  uint32_t r;
  asm volatile("rsr.ccount %0" : "=a"(r));
  return r;
}

/* Raw systimer read: 16 MHz, XTAL-derived, no driver, no flash. */
static inline uint64_t IRAM_ATTR sysTicks(void) {
  REG_WRITE(SYSTIMER_UNIT0_OP_REG, SYSTIMER_TIMER_UNIT0_UPDATE);
  while (!(REG_READ(SYSTIMER_UNIT0_OP_REG) & SYSTIMER_TIMER_UNIT0_VALUE_VALID)) {
  }
  uint32_t lo = REG_READ(SYSTIMER_UNIT0_VALUE_LO_REG);
  uint32_t hi = REG_READ(SYSTIMER_UNIT0_VALUE_HI_REG);
  return ((uint64_t)hi << 32) | lo;
}

/* CPU MHz over a ~10 ms window. Safe with caches off and interrupts off. */
static uint32_t IRAM_ATTR measureRaw(void) {
  uint64_t t0 = sysTicks();
  uint32_t c0 = ccount();
  while (sysTicks() - t0 < 160000) { /* 160000 / 16 MHz = 10 ms */
  }
  uint64_t t1 = sysTicks();
  uint32_t c1 = ccount();
  /* cycles / us; ticks are 1/16 us each */
  return (uint32_t)((uint64_t)(c1 - c0) * 16u / (t1 - t0));
}

uint32_t clkMeasureMhz(void) {
  portDISABLE_INTERRUPTS();
  uint32_t mhz = measureRaw();
  portENABLE_INTERRUPTS();
  return mhz;
}

static void IRAM_ATTR dbiasSet(uint32_t dig, uint32_t rtc) {
  if (dig > 31) dig = 31;
  if (rtc > 31) rtc = 31;
  regi2c_ctrl_write_reg_mask(DIG_REG_BLOCK, DIG_REG_HOST, DIG_REG_EXT_DIG_DREG,
                             4, 0, (uint8_t)dig);
  regi2c_ctrl_write_reg_mask(DIG_REG_BLOCK, DIG_REG_HOST, DIG_REG_EXT_RTC_DREG,
                             4, 0, (uint8_t)rtc);
  esp_rom_delay_us(50);
}

/* Caller must hold the flash guard (caches off, other CPU stalled).
 * mult == 0: stock config for pllMhz (320 or 480).
 * mult >  0: run the stock 480 calibration, then rewrite the feedback divider
 *            to XTAL*mult -- 480-mode divider semantics stay in force, so
 *            CPUPERIOD_SEL=2 gives mult*40/2 MHz and every peripheral tap
 *            scales by mult/12. */
static void IRAM_ATTR pllSwitch(int pllMhz, int mult, int periodSel,
                                int cpuMhz, int dbiasExtra) {
  /* Park the CPU on the crystal while the PLL retrains. */
  REG_SET_FIELD(SYSTEM_SYSCLK_CONF_REG, SYSTEM_SOC_CLK_SEL, 0);
  ets_update_cpu_frequency(40);
  esp_rom_delay_us(5);
  s_stage = 2;
  /* Voltage before frequency. */
  dbiasSet(g_dig_dbias_pvt_240m + dbiasExtra, g_rtc_dbias_pvt_240m + dbiasExtra);
  s_stage = 3;
  /* Power-cycle the BBPLL before retraining it -- calibration never completes
   * on a live PLL (measured: the CAL_DONE poll hangs until TG1WDT resets the
   * chip). This mirrors rtc_clk_bbpll_disable()/enable(), both static. */
  SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG,
                    RTC_CNTL_BB_I2C_FORCE_PD | RTC_CNTL_BBPLL_FORCE_PD |
                        RTC_CNTL_BBPLL_I2C_FORCE_PD);
  esp_rom_delay_us(10);
  CLEAR_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG,
                      RTC_CNTL_BB_I2C_FORCE_PD | RTC_CNTL_BBPLL_FORCE_PD |
                          RTC_CNTL_BBPLL_I2C_FORCE_PD);
  esp_rom_delay_us(10);
  s_stage = 4;
  /* Retrains the PLL and sets SYSTEM_PLL_FREQ_SEL to match pllMhz. */
  rtc_clk_bbpll_configure(40, pllMhz);
  if (mult > 0) {
    regi2c_ctrl_write_reg(BBPLL_BLOCK, BBPLL_HOST, BBPLL_OC_DIV_7_0,
                          (uint8_t)(mult - 4));
    esp_rom_delay_us(300); /* let the loop re-lock at the new multiple */
  }
  s_stage = 5;
  REG_SET_FIELD(SYSTEM_CPU_PER_CONF_REG, SYSTEM_CPUPERIOD_SEL, periodSel);
  REG_SET_FIELD(SYSTEM_SYSCLK_CONF_REG, SYSTEM_PRE_DIV_CNT, 0);
  REG_SET_FIELD(SYSTEM_SYSCLK_CONF_REG, SYSTEM_SOC_CLK_SEL, 1); /* PLL */
  ets_update_cpu_frequency(cpuMhz);
  esp_rom_delay_us(50);
  s_stage = 6;
}

/* PSRAM + multiplier workout at the new clock: fill a PSRAM buffer, hash it
 * back, compare with the same hash computed register-only. Only meaningful
 * once the caches are running under the 320 PLL (hold path). */
static bool stressOk(void) {
  enum { N = 16384 };
  uint32_t *a = heap_caps_malloc(N * 4, MALLOC_CAP_SPIRAM);
  if (a == NULL) {
    a = malloc(N * 4);
    if (a == NULL) return true; /* nothing to test with; gameplay will tell */
  }
  uint32_t h1 = 0x811c9dc5u, h2 = 0x811c9dc5u;
  for (int i = 0; i < N; i++) a[i] = (uint32_t)i * 2654435761u;
  for (int i = 0; i < N; i++) {
    h1 ^= a[i];
    h1 *= 16777619u;
  }
  for (int i = 0; i < N; i++) {
    h2 ^= (uint32_t)i * 2654435761u;
    h2 *= 16777619u;
  }
  free(a);
  return h1 == h2;
}

/* The guarded window in one IRAM function: the flash guard disables the
 * caches, so from that call until re-enable, every instruction executed must
 * come from IRAM -- including the code BETWEEN the helper calls (noinline,
 * or GCC folds this into its flash-resident caller and drops the section).
 * Returns true if it stayed at the target (stayIfSane and measurement sane
 * for a 320 target). */
static bool __attribute__((noinline)) IRAM_ATTR
excursion(int pllMhz, int mult, int periodSel, int cpuMhz, int dbiasExtra,
          bool stayIfSane, uint32_t *outMhz) {
  spi_flash_disable_interrupts_caches_and_other_cpu();
  pllSwitch(pllMhz, mult, periodSel, cpuMhz, dbiasExtra);
  uint32_t oc = measureRaw();
  s_stage = 7;
  s_stageMhz = oc;
  bool sane = oc >= (uint32_t)cpuMhz - 20 && oc <= (uint32_t)cpuMhz + 20;
  bool stay = stayIfSane && sane;
  if (!stay) {
    pllSwitch(480, 0, 2, 240, 0);
  }
  s_stage = 8;
  spi_flash_enable_interrupts_caches_and_other_cpu();
  *outMhz = oc;
  return stay;
}

/* Same shape, for reverting after a failed hold-mode stress test. */
static void __attribute__((noinline)) IRAM_ATTR revertTo240(void) {
  spi_flash_disable_interrupts_caches_and_other_cpu();
  pllSwitch(480, 0, 2, 240, 0);
  spi_flash_enable_interrupts_caches_and_other_cpu();
}

bool clkAt320(void) { return s_at320; }

/* Drop back to stock BEFORE any flash/NVS write. The overdriven BBPLL also
 * overdrives the SPI flash clock (+8%), and writes at that speed have
 * corrupted the cart image in testing. Pair with clkFlashRestore() after the
 * write to re-engage the hold if the auto setting is still on. */
static bool s_guardDropped = false;
void clkFlashGuard(void) {
  if (!s_at320) {
    return;
  }
  revertTo240();
  s_at320 = false;
  s_holdMagic = 0;
  s_guardDropped = true;
  printf("CLK: dropped to stock 240 for a flash write\n");
}

void clkFlashRestore(void) {
  if (!s_guardDropped) {
    return;
  }
  s_guardDropped = false;
  if (clkAutoGet()) {
    clkHoldNow();
  }
}

/* Persistent "switch to 278 after the game starts" flag. Unlike the one-shot
 * hold, this survives reboots: the switch happens AFTER all drivers came up
 * at stock 240, which is the order proven to work. */
bool clkAutoGet(void) {
  nvs_handle_t h;
  uint8_t v = 0; /* overclock OFF by default: suspected of corrupting
                  * PSRAM-resident VRAM over minutes (progressive sprite/
                  * palette decay). Opt-in via settings until cleared. */
  if (nvs_open("clk", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, "auto278", &v);
    nvs_close(h);
  }
  return v != 0;
}

void clkAutoSet(bool on) {
  clkFlashGuard(); /* never write NVS while overclocked */
  nvs_handle_t h;
  if (nvs_open("clk", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "auto278", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
  printf("CLK: auto-278 %s (takes effect when a game starts)\n",
         on ? "ON" : "OFF");
  clkFlashRestore(); /* back to the hold if it was dropped and still wanted */
}

/* Mid-game switch to the proven 278 MHz point (VCO x14), no reboot: every
 * driver keeps the divider values it computed at 240, so their physical
 * clocks scale by 557/480 -- audio +16% pitch if the codec rides the MCLK
 * jump, LCD SPI 46 MHz, SD +16%. Isolates "the overclock" from "drivers
 * initialized under the overclock". USB serial dies here. */
bool clkHoldNow(void) {
  /* x13 = 260 MHz, NOT x14/278: at x14 the PSRAM runs 16%% over spec and the
   * renderer ring now lives there -- measured as an instant INT_WDT crash
   * loop on every game start. x13 keeps PSRAM at 86.7 MHz (+8%%), stable. */
  printf("CLK: live-switching to 260 MHz NOW -- serial dies, watch the LCD\n");
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(100));
  osSerialMarkDead();
  uint32_t oc = 0;
  bool stayed = excursion(480, 13, 2, 260, 2, true, &oc);
  if (stayed) {
    s_holdMagic = CLK_MAGIC;
    s_holdMhz = oc;
    if (!stressOk()) {
      revertTo240();
      s_holdMagic = 0;
      return false;
    }
    s_at320 = true;
    return true;
  }
  return false;
}

void clkRequest320(void) {
  nvs_handle_t h;
  if (nvs_open("clk", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "hold320", 1);
    nvs_commit(h);
    nvs_close(h);
  }
  printf("CLK: rebooting into a 320 MHz hold. USB serial dies with the 480 PLL;"
         " fps proof is the LCD overlay. Any reset returns to 240.\n");
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}

bool clkProbeBoot(void) {
  /* Report on a previous hold session first. */
  if (s_holdMagic == CLK_MAGIC) {
    printf("CLK: previous boot HELD %u MHz, ended by reset reason %d\n",
           (unsigned)s_holdMhz, (int)esp_reset_reason());
    s_holdMagic = 0;
  }

  /* One-shot: clear the flag before switching, so a crash at 320 boots back
   * into stock 240 with no button-holding ceremony. */
  uint8_t hold = 0;
  nvs_handle_t h;
  if (nvs_open("clk", NVS_READWRITE, &h) == ESP_OK) {
    nvs_get_u8(h, "hold320", &hold);
    if (hold) {
      nvs_erase_key(h, "hold320");
      nvs_commit(h);
    }
    nvs_close(h);
  }

  /* Candidate table. Values are copied into locals before entering the
   * guarded window (a static const table would live in flash .rodata --
   * unreadable with the caches off). Bump CAND_VER when this table changes:
   * results persist in RTC RAM across reboots AND reflashes. */
  struct cand { int pll, mult, sel, cpu, dbias; };
  const struct cand cands[] = {
      /* div-by-1 in 320-mode is proven DEAD (wedges at any voltage, CAND_VER
       * 1); the working lever is overdriving the VCO under 480-mode divider
       * semantics. mult=12 is stock 480 through the custom path: sanity. */
      {480, 12, 2, 240, 0},
      {480, 13, 2, 260, 2},
      {480, 14, 2, 280, 2},
      {480, 15, 2, 300, 3},
      {480, 16, 2, 320, 4},
  };
  enum { NCAND = sizeof(cands) / sizeof(cands[0]) };
#define CAND_VER 2
  const uint32_t candMagic = 0xC1A20000u | (CAND_VER << 8) | NCAND;
#define RES_WEDGED 0xDEADu

  if (s_candMagic != candMagic) {
    /* Cold boot wiped RTC RAM. Restore a finished sweep from NVS -- the
     * high candidates can WEDGE the chip (watchdog reboot), so re-probing
     * on every power cycle costs boots and must never happen. */
    s_candMagic = candMagic;
    s_candIdx = 0;
    for (int i = 0; i < 8; i++) s_candRes[i] = 0;
    nvs_handle_t ch;
    if (nvs_open("clk", NVS_READONLY, &ch) == ESP_OK) {
      uint32_t m = 0;
      size_t len = sizeof(s_candRes);
      if (nvs_get_u32(ch, "sweepmag", &m) == ESP_OK && m == candMagic &&
          nvs_get_blob(ch, "sweepres", s_candRes, &len) == ESP_OK) {
        s_candIdx = NCAND;
        printf("CLK: sweep results restored from NVS\n");
      }
      nvs_close(ch);
    }
  }

  if (s_probeArmed == CLK_PROBE_MAGIC) {
    s_probeArmed = 0;
    printf("CLK: candidate %u (pll%d x%d sel%d dbias+%d) WEDGED the chip at "
           "stage %u (reset reason %d)\n",
           (unsigned)s_candIdx, cands[s_candIdx].pll, cands[s_candIdx].mult,
           cands[s_candIdx].sel, cands[s_candIdx].dbias, (unsigned)s_stage,
           (int)esp_reset_reason());
    s_candRes[s_candIdx] = RES_WEDGED;
    s_candIdx++;
  }

  uint32_t base = clkMeasureMhz();
  while (s_candIdx < NCAND) {
    const struct cand c = cands[s_candIdx];
    printf("CLK: base %u MHz, trying candidate %u: pll%d x%d sel%d -> %d MHz "
           "(dbias+%d)...\n",
           (unsigned)base, (unsigned)s_candIdx, c.pll, c.mult, c.sel, c.cpu,
           c.dbias);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100)); /* let USB ship that line first */
    s_probeArmed = CLK_PROBE_MAGIC;
    s_stage = 1;
    s_stageMhz = 0;
    uint32_t oc = 0;
    excursion(c.pll, c.mult, c.sel, c.cpu, c.dbias, false, &oc);
    s_probeArmed = 0;
    s_candRes[s_candIdx] = oc;
    printf("CLK: candidate %u measured %u MHz (target %d), restored %u MHz\n",
           (unsigned)s_candIdx, (unsigned)oc, c.cpu,
           (unsigned)clkMeasureMhz());
    s_candIdx++;
  }

  /* Sweep just finished (or was already done): persist it. */
  {
    nvs_handle_t ch;
    if (nvs_open("clk", NVS_READWRITE, &ch) == ESP_OK) {
      uint32_t m = 0;
      if (nvs_get_u32(ch, "sweepmag", &m) != ESP_OK || m != candMagic) {
        nvs_set_u32(ch, "sweepmag", candMagic);
        nvs_set_blob(ch, "sweepres", s_candRes, sizeof(s_candRes));
        nvs_commit(ch);
      }
      nvs_close(ch);
    }
  }
  printf("CLK: probe table:");
  for (int i = 0; i < NCAND; i++) {
    printf(" [%d: x%d/sel%d -> %u%s]", i, cands[i].mult, cands[i].sel,
           (unsigned)s_candRes[i], s_candRes[i] == RES_WEDGED ? " WEDGED" : "");
  }
  printf("\n");

  /* Hold: the fastest overclock candidate that measured sane. */
  uint32_t best = 0;
  int bestIdx = -1;
  for (int i = 0; i < NCAND; i++) {
    if (cands[i].mult > 13) continue; /* x14+ = PSRAM ring crash, see above */
    if (cands[i].cpu > 240 && s_candRes[i] != RES_WEDGED &&
        s_candRes[i] >= (uint32_t)cands[i].cpu - 20 &&
        s_candRes[i] <= (uint32_t)cands[i].cpu + 20 && s_candRes[i] > best) {
      best = s_candRes[i];
      bestIdx = i;
    }
  }
  if (hold && bestIdx >= 0) {
    printf("CLK: holding %u MHz now -- see you on the LCD\n", (unsigned)best);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    osSerialMarkDead();
    uint32_t oc = 0;
    bool stayed = excursion(cands[bestIdx].pll, cands[bestIdx].mult,
                            cands[bestIdx].sel, cands[bestIdx].cpu,
                            cands[bestIdx].dbias, true, &oc);
    if (stayed) {
      s_holdMagic = CLK_MAGIC;
      s_holdMhz = oc;
      if (!stressOk()) {
        revertTo240();
        s_holdMagic = 0;
        printf("CLK: 320 held but PSRAM stress FAILED -- reverted to 240\n");
        return false;
      }
      s_at320 = true;
      return true;
    }
  } else if (hold) {
    printf("CLK: hold requested but no sane 320 candidate -- staying at 240\n");
  }
  return false;
}
