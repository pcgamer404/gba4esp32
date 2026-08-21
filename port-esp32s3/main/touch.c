/* FT6336 capacitive touch, on the I2C bus shared with the audio codec.
 *
 * Confirmed present by the bus scan (0x38 alongside the codec's 0x18), on the
 * schematic's pins: SCL 15, SDA 16, INT 17, RST 18.
 *
 * The controller reports coordinates in the panel's NATIVE portrait frame
 * (240 wide x 320 tall). The display is driven landscape (MADCTL 0x28 = MV|BGR,
 * 320x240), so the axes have to be swapped to match what is on screen. Raw
 * values are logged too, so the mapping can be corrected against real taps
 * rather than assumed.
 */
#include "touch.h"

#include "audio.h" /* PIN_I2C_*, I2C_PORT, audioI2cProbe */
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os.h"
#include <stdlib.h>

#define TAG "TOUCH"

#define FT6336_ADDR 0x38
#define PIN_CTP_INT 17
#define PIN_CTP_RST 18

#define REG_TD_STATUS 0x02
#define REG_P1_XH 0x03

/* Native panel geometry, before the landscape rotation. */
#define PANEL_NATIVE_W 240
#define PANEL_NATIVE_H 320

static bool present;

/* Measured mapping from controller coordinates to screen coordinates. */
static touchCal cal;

void touchSetCal(const touchCal *c) { cal = *c; }
const touchCal *touchGetCal(void) { return &cal; }

/* Read a raw point, waiting for a press then a release. */
static bool touchWaitRaw(int *rx, int *ry) {
  touchPoint p;
  /* wait for release first, so a held finger does not double-register */
  int guard = 0;
  while (touchRead(&p) && guard < 3000) {
    vTaskDelay(pdMS_TO_TICKS(10));
    guard += 10;
  }
  guard = 0;
  while (!touchRead(&p)) {
    vTaskDelay(pdMS_TO_TICKS(10));
    guard += 10;
    if (guard > 8000) {
      return false;  /* nobody is tapping: skip rather than block the boot */
    }
  }
  int lx = p.rawX, ly = p.rawY;
  while (touchRead(&p)) {
    lx = p.rawX;
    ly = p.rawY;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  *rx = lx;
  *ry = ly;
  return true;
}

/* Three reference taps: top-left, top-right, bottom-left.
 *
 * TL->TR moves only in screen X, TL->BL only in screen Y, so whichever raw axis
 * changes across each pair IS that screen axis -- which detects the swap and
 * the direction of each axis without assuming anything about MADCTL.
 */
bool touchCalibrate(int tlx, int tly, int trx, int try_, int blx, int bly,
                    void (*showTarget)(int, int)) {
  int r1x, r1y, r2x, r2y, r3x, r3y;

  showTarget(tlx, tly);
  if (!touchWaitRaw(&r1x, &r1y)) return false;
  showTarget(trx, try_);
  if (!touchWaitRaw(&r2x, &r2y)) return false;
  showTarget(blx, bly);
  if (!touchWaitRaw(&r3x, &r3y)) return false;

  int dHorizX = r2x - r1x, dHorizY = r2y - r1y;  /* moving along screen X */
  int dVertX = r3x - r1x, dVertY = r3y - r1y;    /* moving along screen Y */

  cal.swap = (abs(dHorizY) > abs(dHorizX));

  int hx = cal.swap ? dHorizY : dHorizX;
  int vy = cal.swap ? dVertX : dVertY;
  if (hx == 0 || vy == 0) {
    ESP_LOGE(TAG, "degenerate calibration (hx=%d vy=%d)", hx, vy);
    return false;
  }

  cal.xNum = trx - tlx;
  cal.xDen = hx;
  cal.yNum = bly - tly;
  cal.yDen = vy;

  int a1x = cal.swap ? r1y : r1x;
  int a1y = cal.swap ? r1x : r1y;
  cal.xOff = tlx - (a1x * cal.xNum) / cal.xDen;
  cal.yOff = tly - (a1y * cal.yNum) / cal.yDen;
  cal.valid = 1;

  ESP_LOGI(TAG, "calibrated: swap=%d x=(%d/%d)+%d y=(%d/%d)+%d", cal.swap,
           cal.xNum, cal.xDen, cal.xOff, cal.yNum, cal.yDen, cal.yOff);
  ESP_LOGI(TAG, "  refs raw TL=(%d,%d) TR=(%d,%d) BL=(%d,%d)", r1x, r1y, r2x,
           r2y, r3x, r3y);
  return true;
}

bool touchInit(void) {
  gpio_reset_pin(PIN_CTP_RST);
  gpio_set_direction(PIN_CTP_RST, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_CTP_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(PIN_CTP_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(120)); /* FT6336 needs ~100ms after reset */

  gpio_reset_pin(PIN_CTP_INT);
  gpio_set_direction(PIN_CTP_INT, GPIO_MODE_INPUT);
  gpio_pullup_en(PIN_CTP_INT);

  present = audioI2cProbe(FT6336_ADDR);
  ESP_LOGI(TAG, "FT6336 at 0x%02x: %s", FT6336_ADDR,
           present ? "present" : "NOT FOUND");
  return present;
}

bool touchRead(touchPoint *out) {
  if (!present) {
    return false;
  }
  uint8_t reg = REG_TD_STATUS;
  uint8_t buf[5] = {0};
  if (i2c_master_write_read_device(I2C_PORT, FT6336_ADDR, &reg, 1, buf,
                                   sizeof(buf), pdMS_TO_TICKS(30)) != ESP_OK) {
    return false;
  }

  int points = buf[0] & 0x0F;
  if (points == 0 || points > 2) {
    return false;
  }

  int rawX = ((buf[1] & 0x0F) << 8) | buf[2];
  int rawY = ((buf[3] & 0x0F) << 8) | buf[4];

  /* Apply the stored calibration rather than a hand-derived rotation.
   * I guessed this transform twice from MADCTL and got it wrong both times;
   * three tapped reference points measure it instead. cal.valid is false until
   * touchCalibrate() has run. */
  int sx, sy;
  if (cal.valid) {
    int ax = cal.swap ? rawY : rawX;
    int ay = cal.swap ? rawX : rawY;
    sx = cal.xOff + (ax * cal.xNum) / cal.xDen;
    sy = cal.yOff + (ay * cal.yNum) / cal.yDen;
  } else {
    sx = rawY;
    sy = rawX;
  }

  if (sx < 0) sx = 0;
  if (sx >= LCD_W) sx = LCD_W - 1;
  if (sy < 0) sy = 0;
  if (sy >= LCD_H) sy = LCD_H - 1;

  out->x = sx;
  out->y = sy;
  out->rawX = rawX;
  out->rawY = rawY;
  return true;
}

/* Debounced tap: waits for release so a single touch yields one event. */
bool touchTap(touchPoint *out) {
  touchPoint p;
  if (!touchRead(&p)) {
    return false;
  }
  touchPoint last = p;
  int held = 0;
  while (touchRead(&p) && held < 3000) {
    last = p;
    vTaskDelay(pdMS_TO_TICKS(10));
    held += 10;
  }
  if (held < 20) {
    return false; /* too short to be real */
  }
  *out = last;
  return true;
}
