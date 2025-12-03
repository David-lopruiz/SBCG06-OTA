/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ccs811.h"

#define SDA_PIN 21
#define SCL_PIN 22

void i2c_init() {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = SCL_PIN,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0));
}

void app_main() {

    printf("Iniciando I2C y CCS811...\n");

    i2c_init();
    ccs811_init();
    ccs811_app_start();

    while (1) {
        uint8_t status = ccs811_read_status();
        printf("STATUS = 0x%02X\n", status);

        ccs811_data_t gas = ccs811_read();

        printf("eCO2 = %d ppm | TVOC = %d ppb\n",
               gas.eco2, gas.tvoc);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}




