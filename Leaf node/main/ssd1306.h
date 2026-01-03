#ifndef SSD1306_H_
#define SSD1306_H_

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c.h"

// ==== OLED SSD1306 ====
#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64
#define SSD1306_I2C_ADDRESS 0x3C

// ==== I2C config ====
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_FREQ_HZ 100000

typedef struct {
    int width;
    int height;
} SSD1306_t;

// ==== API ====
void ssd1306_init(SSD1306_t *dev);
void ssd1306_clear(SSD1306_t *dev);
void ssd1306_display_text(SSD1306_t *dev, int row, const char *text, bool invert);

#endif /* SSD1306_H_ */
