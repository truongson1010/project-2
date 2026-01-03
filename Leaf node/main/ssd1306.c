#include <string.h>
#include "ssd1306.h"
#include "esp_log.h"
#include "font8x8_basic.h"

static const char *TAG = "SSD1306";

// ==== Hàm gửi command ====
static void ssd1306_send_cmd(uint8_t cmd) {
    uint8_t buffer[2] = {0x00, cmd};
    i2c_master_write_to_device(I2C_MASTER_NUM, SSD1306_I2C_ADDRESS,
                               buffer, 2, 1000 / portTICK_PERIOD_MS);
}

// ==== Hàm gửi data ====
static void ssd1306_send_data(uint8_t *data, size_t len) {
    uint8_t buffer[len + 1];
    buffer[0] = 0x40;
    memcpy(&buffer[1], data, len);
    i2c_master_write_to_device(I2C_MASTER_NUM, SSD1306_I2C_ADDRESS,
                               buffer, len+1, 1000 / portTICK_PERIOD_MS);
}

// ==== Khởi tạo OLED ====
void ssd1306_init(SSD1306_t *dev) {
    dev->width = SSD1306_WIDTH;
    dev->height = SSD1306_HEIGHT;

    // Init I2C
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    // Sequence init
    ssd1306_send_cmd(0xAE); // Display OFF
    ssd1306_send_cmd(0x20); // Memory addressing mode
    ssd1306_send_cmd(0x00); // Horizontal addressing
    ssd1306_send_cmd(0x40); // Start line
    ssd1306_send_cmd(0xA1); // Segment remap
    ssd1306_send_cmd(0xC8); // COM scan direction
    ssd1306_send_cmd(0x81); // Contrast
    ssd1306_send_cmd(0x7F);
    ssd1306_send_cmd(0xA6); // Normal display
    ssd1306_send_cmd(0xA8); // Multiplex
    ssd1306_send_cmd(0x3F);
    ssd1306_send_cmd(0xD3); // Display offset
    ssd1306_send_cmd(0x00);
    ssd1306_send_cmd(0xD5); // Clock divide
    ssd1306_send_cmd(0x80);
    ssd1306_send_cmd(0xD9); // Precharge
    ssd1306_send_cmd(0xF1);
    ssd1306_send_cmd(0xDA); // COM pins
    ssd1306_send_cmd(0x12);
    ssd1306_send_cmd(0xDB); // VCOM detect
    ssd1306_send_cmd(0x40);
    ssd1306_send_cmd(0x8D); // Charge pump
    ssd1306_send_cmd(0x14);
    ssd1306_send_cmd(0xAF); // Display ON

    ESP_LOGI(TAG, "SSD1306 init OK");
}

// ==== Clear screen ====
void ssd1306_clear(SSD1306_t *dev) {
    uint8_t zero[SSD1306_WIDTH];
    memset(zero, 0x00, sizeof(zero));
    for (int page = 0; page < (SSD1306_HEIGHT / 8); page++) {
        ssd1306_send_cmd(0xB0 + page);
        ssd1306_send_cmd(0x00);
        ssd1306_send_cmd(0x10);
        ssd1306_send_data(zero, SSD1306_WIDTH);
    }
}

// ==== Hiển thị text ====
void ssd1306_display_text(SSD1306_t *dev, int row, const char *text, bool invert) {
    if (row >= (SSD1306_HEIGHT / 8)) return;
    int len = strlen(text);
    if (len > 16) len = 16;

    ssd1306_send_cmd(0xB0 + row);
    ssd1306_send_cmd(0x00);
    ssd1306_send_cmd(0x10);

    uint8_t buffer[len*8];
    for (int i = 0; i < len; i++) {
        memcpy(&buffer[i*8], font8x8_basic_tr[(uint8_t)text[i]], 8);
        if (invert) {
            for (int j=0;j<8;j++) buffer[i*8+j] = ~buffer[i*8+j];
        }
    }
    ssd1306_send_data(buffer, len*8);
}
