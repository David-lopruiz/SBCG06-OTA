#pragma once
#include <stdint.h>

#define CCS811_ADDR 0x5B   // Dirección correcta del Pmod AQS

typedef struct {
    uint16_t eco2;
    uint16_t tvoc;
    uint8_t status;
} ccs811_data_t;

void ccs811_init();
ccs811_data_t ccs811_read();
void ccs811_app_start();
uint8_t ccs811_read_status();
