#pragma once

#include "driver/spi_master.h"

/* Handheld hardware configuration */

/* LCD: ILI9341, 320x240 landscape */
#define PANEL_ILI9341     1
#define LCD_W 320
#define LCD_H 240
#define PIN_LCD_CS 10
#define PIN_LCD_DC 11
#define PIN_SPI0_MOSI 13
#define PIN_SPI0_MISO 9
#define PIN_SPI0_SCLK 12
#define PIN_SYS_RSTN (-1)
#define PIN_LCD_BL 14
#define LCD_SPI_HZ 40000000
#define LCD_HAS_READBACK 0

/* SD card: SPI2, shared with LCD */
#define SD_SPI_HOST SPI2_HOST
#define SD_PIN_MISO 9
#define SD_PIN_MOSI 13
#define SD_PIN_CLK 12
#define SD_PIN_CS 18

/* Buttons: active-low, internal pull-ups */
#define PIN_KEY_UP 8
#define PIN_KEY_DOWN 6
#define PIN_KEY_LEFT 7
#define PIN_KEY_RIGHT 15
#define PIN_KEY_A 47
#define PIN_KEY_B 40
#define PIN_KEY_SELECT 5
#define PIN_KEY_START 2

/* Extra buttons used as GBA L/R */
#define PIN_KEY_L 1
#define PIN_KEY_R 21
