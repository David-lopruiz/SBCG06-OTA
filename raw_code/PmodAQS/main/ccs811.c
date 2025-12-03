#include "driver/i2c.h"
#include "esp_log.h"
#include "ccs811.h"

#define TAG "CCS811"

// Lee un registro de 1 byte
static uint8_t read8(uint8_t reg) {
    uint8_t value = 0;
    i2c_master_write_read_device(I2C_NUM_0, CCS811_ADDR,
                                 &reg, 1, &value, 1,
                                 1000/portTICK_PERIOD_MS);
    return value;
}

// Lee dos valores (eCO2 y TVOC)
ccs811_data_t ccs811_read() {
    ccs811_data_t result = {0};
    uint8_t reg = 0x02;  // ALG_RESULT_DATA
    uint8_t buf[4] = {0};

    i2c_master_write_read_device(I2C_NUM_0, CCS811_ADDR,
                                 &reg, 1, buf, 4,
                                 1000/portTICK_PERIOD_MS);

    result.eco2 = (buf[0] << 8) | buf[1];
    result.tvoc = (buf[2] << 8) | buf[3];

    result.status = ccs811_read_status();

    return result;
}

uint8_t ccs811_read_status() {
    return read8(0x00);  // STATUS register
}

void ccs811_init() {
    ESP_LOGI(TAG, "Inicializando CCS811...");

    // Poner modo 1 medición (0x10)
    uint8_t cmd[2] = { 0x01, 0x10 };  
    i2c_master_write_to_device(I2C_NUM_0, CCS811_ADDR,
                               cmd, 2,
                               1000/portTICK_PERIOD_MS);

    vTaskDelay(pdMS_TO_TICKS(100));
}

void ccs811_app_start() {
    uint8_t cmd = 0xF4;   // APP_START command
    i2c_master_write_to_device(I2C_NUM_0, CCS811_ADDR,
                               &cmd, 1,
                               1000/portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(100));
}

