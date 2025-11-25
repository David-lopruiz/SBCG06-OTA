#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stddef.h>
#include "esp_err.h"

/**
 * Lanza una OTA directa desde una URL dada.
 * Normalmente se usa internamente desde ota_check_for_update().
 */
esp_err_t https_ota(const char *url);

/**
 * Comprueba el manifest remoto, decide si hay nueva versión y,
 * si la hay, guarda pending_version en NVS y ejecuta la OTA.
 */
esp_err_t ota_check_for_update(void);

/**
 * Programa una tarea que ejecuta ota_check_for_update() una vez
 * cada día a la hora/minuto indicados.
 */
esp_err_t ota_schedule_daily(int hour, int minute);

/**
 * Devuelve la última versión confirmada en NVS (clave "last_version").
 */
esp_err_t ota_get_stored_version(char *out, size_t len);

/**
 * Si existe pending_version en NVS, aplica la actualización ahora (llama a esp_restart).
 * Devuelve ESP_OK si encontró pending_version (nota: esp_restart no retorna).
 * Devuelve ESP_ERR_NOT_FOUND si no hay pending_version.
 */
esp_err_t ota_apply_pending_now(void);
#endif // OTA_UPDATE_H
