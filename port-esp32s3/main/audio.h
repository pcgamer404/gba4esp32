#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared I2C bus: ES8311 codec (0x18) and FT6336 touch (0x38). */
#define PIN_I2C_SCL 15
#define PIN_I2C_SDA 16
#define I2C_PORT 0

/* Percent maps to the ES8311's dB scale now (see audioSetVolume): 100% is
 * 0 dB full scale, each percent is 0.5 dB. The old 45%/25% caps were an
 * artifact of a broken linear mapping that made everything inaudible. */
#define AUDIO_MAX_VOLUME_PCT 100
#define AUDIO_DEFAULT_VOLUME_PCT 80

esp_err_t audioI2cInit(void);
bool audioI2cProbe(uint8_t addr);
void audioI2cScan(void);

bool audioInit(int sampleRate);
void audioWrite(const int16_t *pcm, int samples);
void audioSetVolume(int pct);
void audioAmpEnable(bool on);
/* Track emulation speed: continuous slow-motion audio instead of stutter.
 * pllNum/pllDen fold in PLL-overdrive compensation (557/480 held, else 1/1
 * or 480/480). Call once a second with the measured emu speed in deci-fps. */
void audioMatchRate(int emuCentiFps, int pllNum, int pllDen);
void audioTestTone(int ms, int hz);
void audioTestTonePolarity(int ms, int hz);
/* Play a tone and listen through the board's own mic: analog-path verdict. */
void audioDumpRegs(void);
/* Count edges on the I2S pads while a tone plays: electrical output proof. */
void audioProbePins(void);

#ifdef __cplusplus
}
#endif
