#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int x, y;        /* mapped to the landscape screen */
  int rawX, rawY;  /* as reported by the controller, for calibration */
} touchPoint;

/* Affine mapping measured by touchCalibrate(); persisted in NVS. */
typedef struct {
  int valid;
  int swap;               /* raw axes exchanged relative to the screen */
  int xNum, xDen, xOff;
  int yNum, yDen, yOff;
} touchCal;

bool touchCalibrate(int tlx, int tly, int trx, int try_, int blx, int bly,
                    void (*showTarget)(int, int));
void touchSetCal(const touchCal *c);
const touchCal *touchGetCal(void);

bool touchInit(void);
bool touchRead(touchPoint *out);
bool touchTap(touchPoint *out);

#ifdef __cplusplus
}
#endif
