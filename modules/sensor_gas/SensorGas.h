#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"

#define CCS811_ADDR 0x5B   // Dirección del Pmod AQS

typedef struct {
    uint16_t eco2;
    uint16_t tvoc;
    uint8_t  status;
} ccs811_data_t;

/**
 * @brief Inicia el CCS811 y lanza la tarea de lectura periódica.
 * 
 * @param shared_data Puntero a estructura donde se guardarán las últimas lecturas.
 * @return 0 en éxito, <0 en error.
 */
int ccs811_start(ccs811_data_t *shared_data);

/**
 * @brief Copia de forma segura la última lectura a 'out'.
 * 
 * @param out Estructura destino.
 * @param timeout_ms Timeout para el mutex (en ms).
 * @return 0 en éxito, <0 en error.
 */
int ccs811_read_safe(ccs811_data_t *out, TickType_t timeout_ms);

/**
 * @brief Detiene la tarea del CCS811 y libera recursos.
 * 
 * @return 0 en éxito, <0 en error.
 */
int ccs811_stop(void);