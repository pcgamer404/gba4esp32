/* ES8311 audio codec + I2S output, and the shared I2C bus.
 *
 * Pins read off the schematic's ESP32-S3 symbol, NOT the vendor sketch -- the
 * sketch's audio pins (I2S 15/16/17/18/21, I2C 39/38) belong to a different
 * variant, the same trap as its SD pins:
 *
 *   AUDIO_EN  GPIO1     amplifier enable, active high
 *   I2S_MCK   GPIO4     master clock to the codec
 *   I2S_SCK   GPIO5     bit clock
 *   I2S_DO    GPIO6     data to codec (playback)
 *   I2S_LRC   GPIO7     word select
 *   I2S_DI    GPIO8     data from codec (microphone)
 *   AU_SCL    GPIO15    shared with the touch controller
 *   AU_SDA    GPIO16    shared with the touch controller
 *
 * The ES8311 sits at I2C 0x18 and the FT6336 touch controller at 0x38, both on
 * that one bus (schematic labels them "IIC Slave device address: 0x18 / 0x38").
 */
#include "audio.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "AUDIO"

#define PIN_AUDIO_EN 1
#define PIN_I2S_MCK 4
#define PIN_I2S_SCK 5
/* Freenove's table names these from the ESP's perspective: I2S_DOUT (GPIO 8)
 * is the ESP's PLAYBACK output into the codec's SDIN; I2S_DINT (GPIO 6) is
 * the mic data coming back. We had them swapped -- the DAC's data pin sat
 * floating, which is perfect silence at any volume on any speaker. */
#define PIN_I2S_DO 8
#define PIN_I2S_LRC 7
#define PIN_I2S_DI 6

#define ES8311_ADDR 0x18
#define I2S_PORT I2S_NUM_0

static bool codecReady;

/* ---------------------------------------------------------------- I2C --- */

esp_err_t audioI2cInit(void) {
  i2c_config_t cfg = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = PIN_I2C_SDA,
      .scl_io_num = PIN_I2C_SCL,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = 400000,
  };
  esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
  if (err != ESP_OK) {
    return err;
  }
  return i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

bool audioI2cProbe(uint8_t addr) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_stop(cmd);
  esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(cmd);
  return err == ESP_OK;
}

void audioI2cScan(void) {
  char found[128] = {0};
  int p = 0;
  for (uint8_t a = 3; a < 0x78; a++) {
    if (audioI2cProbe(a)) {
      p += snprintf(found + p, sizeof(found) - p, "0x%02x ", a);
    }
  }
  ESP_LOGI(TAG, "I2C scan on SCL=%d SDA=%d: %s", PIN_I2C_SCL, PIN_I2C_SDA,
           p ? found : "(nothing responded)");
}

static esp_err_t esWrite(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return i2c_master_write_to_device(I2C_PORT, ES8311_ADDR, buf, 2,
                                    pdMS_TO_TICKS(50));
}

static uint8_t esRead(uint8_t reg) {
  uint8_t v = 0;
  i2c_master_write_read_device(I2C_PORT, ES8311_ADDR, &reg, 1, &v, 1,
                               pdMS_TO_TICKS(50));
  return v;
}

/* --------------------------------------------------------------- codec --- */

/* ES8311 playback setup, faithful port of Espressif's es8311 component (the
 * one Freenove's own Music sketch ships for this exact board). The previous
 * hand-rolled init had three silence-guaranteeing bugs, mic-proven one by
 * one: reg01=0x30 left the internal DAC clock gates CLOSED (must be 0x3F),
 * reg09/0A=0x00 set 24-bit SDP against our 16-bit stream (must be 0x0C),
 * and there was no reset sequence. Divider coefficients are the ratio-256
 * table row (MCLK = 256*fs), valid at any absolute fs including 47872. */
static bool es8311Init(int sampleRate) {
  if (!audioI2cProbe(ES8311_ADDR)) {
    ESP_LOGE(TAG, "no ES8311 at 0x%02x", ES8311_ADDR);
    return false;
  }

  /* Reset to defaults, then power on the chip state machine. */
  esWrite(0x00, 0x1F);
  vTaskDelay(pdMS_TO_TICKS(20));
  esWrite(0x00, 0x00);
  esWrite(0x00, 0x80);

  /* Clock manager: MCLK from the MCLK pad, every internal clock gate OPEN. */
  esWrite(0x01, 0x3F);
  /* MCLK/LRCK ratio 256 coefficient row: pre_div 1, pre_multi x1, fs_mode
   * single, ADC/DAC OSR 0x10, adc/dac_div 1, bclk_div 4, LRCK dividers 255
   * (the last two matter only in master mode; set them anyway). */
  esWrite(0x02, 0x00);
  esWrite(0x03, 0x10);
  esWrite(0x04, 0x10);
  esWrite(0x05, 0x00);
  esWrite(0x06, 0x03);
  esWrite(0x07, 0x00);
  esWrite(0x08, 0xFF);

  /* Format: slave (reg00 bit6 clear, already 0), 16-bit I2S in and out. */
  esWrite(0x09, 0x0C);
  esWrite(0x0A, 0x0C);

  /* Analog power-up, straight from es8311_init(). */
  esWrite(0x0D, 0x01);  /* power up analog circuitry */
  esWrite(0x0E, 0x02);  /* enable analog PGA, ADC modulator */
  esWrite(0x12, 0x00);  /* power up DAC */
  esWrite(0x13, 0x10);  /* enable output to HP drive */
  esWrite(0x1C, 0x6A);  /* ADC EQ bypass, cancel DC offset */
  esWrite(0x37, 0x08);  /* bypass DAC equalizer */

  esWrite(0x31, 0x00);  /* DAC unmuted (bits 5/6 set would mute) */
  esWrite(0x32, 0x00);  /* volume set by audioSetVolume() */

  uint8_t chk = esRead(0x00);
  ESP_LOGI(TAG, "ES8311 up (reg00=0x%02x) at %d Hz", chk, sampleRate);
  return true;
}

/* 0..100 -> ES8311 register 0x32 (0x00 mute .. 0xFF max).
 * Deliberately capped: full scale into a small speaker is unpleasant and this
 * gets tested in a room with other people in it. */
void audioSetVolume(int pct) {
  if (!codecReady) {
    return;
  }
  if (pct < 0) pct = 0;
  if (pct > AUDIO_MAX_VOLUME_PCT) pct = AUDIO_MAX_VOLUME_PCT;
  /* Reg 0x32 is 0.5 dB per step with 0xBF = 0 dB -- NOT linear-in-amplitude.
   * The old pct*255/100 mapping put 25% at -64 dB and even "max" (45%) at
   * -38 dB: inaudible in a room, only a mic against the cone picked it up.
   * Map 1 pct = 1 step (0.5 dB): 100% = 0 dB, 80% = -10 dB, 0% = mute. */
  uint8_t reg = pct == 0 ? 0 : (uint8_t)(191 - (100 - pct));
  esWrite(0x32, reg);
  ESP_LOGI(TAG, "volume %d%% (reg32=0x%02x = %.1f dB)", pct, reg,
           (reg - 191) / 2.0);
}

/* ----------------------------------------------------------------- I2S --- */

bool audioInit(int sampleRate) {
  gpio_reset_pin(PIN_AUDIO_EN);
  gpio_set_direction(PIN_AUDIO_EN, GPIO_MODE_OUTPUT);
  /* SC8002B SHUTDOWN pin: HIGH = amp muted, LOW = playing. Freenove's own
   * sketches drive it LOW and never touch it again; the schematic label
   * "AUDIO_EN"/"AP_ENABLE" is a lie about polarity. Mute during init. */
  gpio_set_level(PIN_AUDIO_EN, 1);

  if (!es8311Init(sampleRate)) {
    return false;
  }
  codecReady = true;

  i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
      .sample_rate = sampleRate,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0,
      /* The emulator hands over ~800 stereo frames in one burst per emulated
       * frame; give the DMA enough slack to absorb a burst on top of an
       * almost-full ring so matched-rate playback doesn't drop chunks. */
      .dma_buf_count = 10,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_driver_install failed");
    return false;
  }

  i2s_pin_config_t pins = {
      .mck_io_num = PIN_I2S_MCK,
      .bck_io_num = PIN_I2S_SCK,
      .ws_io_num = PIN_I2S_LRC,
      .data_out_num = PIN_I2S_DO,
      .data_in_num = PIN_I2S_DI,
  };
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_set_pin failed");
    return false;
  }

  audioSetVolume(AUDIO_DEFAULT_VOLUME_PCT);
  gpio_set_level(PIN_AUDIO_EN, 0); /* amp on last (LOW = playing) */
  vTaskDelay(pdMS_TO_TICKS(300));  /* 8002 bypass cap charge before use */
  ESP_LOGI(TAG, "audio ready: I2S MCK=%d SCK=%d DO=%d LRC=%d, amp SHUTDOWN=%d(LOW=on)",
           PIN_I2S_MCK, PIN_I2S_SCK, PIN_I2S_DO, PIN_I2S_LRC, PIN_AUDIO_EN);
  return true;
}

/* Non-blocking: drop samples rather than stall the emulator if the DMA queue
 * is full. A brief dropout beats halving the frame rate. */
void audioWrite(const int16_t *pcm, int samples) {
  if (!codecReady || samples <= 0) {
    return;
  }
  size_t wrote = 0;
  i2s_write(I2S_PORT, pcm, (size_t)samples * sizeof(int16_t), &wrote, 0);
}

/* Play a 440Hz sine straight to the codec.
 *
 * Removes the emulator from the question entirely: if this is audible the whole
 * chain (I2C config, MCLK, I2S, amp enable, speaker) is good and any silence
 * during a game is a supply-rate problem, not a wiring one. If it is silent the
 * fault is in the codec setup.
 */
/* The SC8002B's SHUTDOWN pin (schematic U6 pin 1) carries a 10K pull-up to
 * 3V3, so its idle state is "high". Whether high means enabled or shut down is
 * not something to guess at -- play the same tone at both polarities and let
 * the ear decide. */
void audioTestTonePolarity(int ms, int hz) {
  audioSetVolume(AUDIO_MAX_VOLUME_PCT);  /* the mic test needs all the SNR it can get */
  ESP_LOGI(TAG, "=== tone A: SHUTDOWN HIGH (amp muted) ===");
  gpio_set_level(PIN_AUDIO_EN, 1);
  audioTestTone(ms, hz);
  vTaskDelay(pdMS_TO_TICKS(700));
  ESP_LOGI(TAG, "=== tone B: SHUTDOWN LOW (amp on) ===");
  gpio_set_level(PIN_AUDIO_EN, 0);
  vTaskDelay(pdMS_TO_TICKS(300)); /* bypass cap charge */
  audioTestTone(ms, hz);
  ESP_LOGI(TAG, "=== B should be the audible one ===");
  audioSetVolume(AUDIO_DEFAULT_VOLUME_PCT);
}

void audioTestTone(int ms, int hz) {
  if (!codecReady) {
    ESP_LOGW(TAG, "test tone skipped: codec not initialised");
    return;
  }
  /* 480 samples = 10ms at 48kHz, so any hz that is a multiple of 100 loops
   * seamlessly. The old version looped ONE cycle over 256 samples, which made
   * every "440Hz" test actually a 187Hz rumble a tiny speaker can barely
   * reproduce -- and nearly killed the microphone-based polarity test. */
  static int16_t wave[480 * 2];
  int rate = 48000;
  for (int i = 0; i < 480; i++) {
    int v = (int)(10000.0f * sinf(2.0f * 3.14159265f * hz * i / (float)rate));
    wave[i * 2] = (int16_t)v;
    wave[i * 2 + 1] = (int16_t)v;
  }

  int total = (rate * ms) / 1000;
  int sent = 0;
  ESP_LOGI(TAG, "test tone: %dms @ %dHz, volume %d%%", ms, hz,
           AUDIO_DEFAULT_VOLUME_PCT);
  bool probed = false;
  while (sent < total) {
    size_t wrote = 0;
    i2s_write(I2S_PORT, wave, sizeof(wave), &wrote, pdMS_TO_TICKS(200));
    if (wrote == 0) {
      ESP_LOGE(TAG, "i2s_write wrote nothing -- I2S is not draining");
      break;
    }
    sent += (int)(wrote / 4);
    if (!probed && sent > total / 4) {
      probed = true; /* DMA is saturated: lines should be toggling NOW */
      audioProbePins();
    }
  }
  ESP_LOGI(TAG, "test tone done, %d of %d frames sent", sent, total);
}

/* Dump the registers that decide whether sound comes out at all. */
void audioDumpRegs(void) {
  if (!audioI2cProbe(ES8311_ADDR)) {
    ESP_LOGE(TAG, "ES8311 not responding");
    return;
  }
  /* Chip ID first: 0xFD/0xFE must read 0x83/0x11 on a real ES8311. Anything
   * else means the board carries a different codec and every register write
   * we make is aimed at the wrong datasheet. */
  ESP_LOGI(TAG, "codec ID: FD=%02x FE=%02x FF=%02x (ES8311 would be 83/11)",
           esRead(0xFD), esRead(0xFE), esRead(0xFF));
  ESP_LOGI(TAG,
           "ES8311 regs: 00=%02x 01=%02x 02=%02x 12=%02x 13=%02x 31=%02x "
           "32=%02x 37=%02x 45=%02x",
           esRead(0x00), esRead(0x01), esRead(0x02), esRead(0x12), esRead(0x13),
           esRead(0x31), esRead(0x32), esRead(0x37), esRead(0x45));
}

/* Electrically verify the I2S lines: enable ONLY the pad input buffer (the
 * IO_MUX FUN_IE bit -- gpio_set_direction would re-route the pad away from
 * the I2S peripheral and sabotage the measurement) and sample the raw
 * GPIO_IN register. A driven clock/data line shows thousands of edges over
 * the window, a dead one ~0. Call while a tone is playing. */
void audioProbePins(void) {
  const int pins[4] = {PIN_I2S_MCK, PIN_I2S_SCK, PIN_I2S_LRC, PIN_I2S_DO};
  const char *names[4] = {"MCK", "SCK", "LRC", "DO"};
  for (int i = 0; i < 4; i++) {
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[pins[i]]);
  }
  int edges[4] = {0};
  uint32_t last = REG_READ(GPIO_IN_REG); /* pins 4/5/7/8 all live here */
  for (int n = 0; n < 200000; n++) {
    uint32_t now = REG_READ(GPIO_IN_REG);
    uint32_t diff = now ^ last;
    for (int i = 0; i < 4; i++) {
      if (diff & (1u << pins[i])) edges[i]++;
    }
    last = now;
  }
  ESP_LOGI(TAG, "pin edges over 200k samples: %s(%d)=%d %s(%d)=%d %s(%d)=%d %s(%d)=%d",
           names[0], pins[0], edges[0], names[1], pins[1], edges[1],
           names[2], pins[2], edges[2], names[3], pins[3], edges[3]);
}

void audioAmpEnable(bool on) {
  gpio_set_level(PIN_AUDIO_EN, on ? 0 : 1); /* SC8002B SHUTDOWN: LOW = on */
}

/* Match the I2S output rate to the emulator's ACTUAL speed. The core
 * produces 801.5 stereo pairs per emulated frame; below full speed that is
 * less than real-time, and at the nominal 47872 Hz the DMA starves every
 * frame -- sound-gap-sound-gap at 30 Hz, the machine-gun stutter. Playing
 * at emuFps/59.73 of the nominal rate makes the stream CONTINUOUS: slow-
 * motion pitch instead of chopping, converging to correct pitch as the
 * emulator approaches full speed.
 *
 * pllNum/pllDen compensate a PLL overdrive (557/480 during the 278 MHz
 * hold): the I2S driver computes dividers against nominal tap frequencies,
 * so ask for the rate scaled down and the physical output lands on target.
 * 5% hysteresis: i2s_set_clk stops the bus briefly, don't thrash it. */
void audioMatchRate(int emuCentiFps, int pllNum, int pllDen) {
  static int curTarget;
  if (!codecReady || emuCentiFps <= 0) {
    return;
  }
  int target = (int)((int64_t)47872 * emuCentiFps / 597); /* 59.7 fps = 597 */
  if (target < 8000) target = 8000;
  if (target > 47872) target = 47872;
  int diff = target > curTarget ? target - curTarget : curTarget - target;
  if (curTarget != 0 && diff * 20 < curTarget) {
    return;
  }
  curTarget = target;
  int askHz = (int)((int64_t)target * pllDen / pllNum);
  i2s_set_clk(I2S_PORT, askHz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}
