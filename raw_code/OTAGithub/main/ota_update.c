#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_app_desc.h"

#include "ota_update.h"
#include "cJSON.h"

#define TAG "ota_update"
#define MANIFEST_URL "https://raw.githubusercontent.com/David-lopruiz/SBCG06-WORKFLOW/main/Versions/latest.json"

esp_err_t https_ota(const char *url)
{
    ESP_LOGI(TAG, "Iniciando OTA segura desde: %s", url);

    esp_http_client_config_t client_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &client_cfg,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA completada correctamente");
    } else {
        ESP_LOGE(TAG, "Error en OTA: %s", esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t http_get(const char *url, char *buffer, size_t max_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Fallo al inicializar cliente HTTP");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo conexión HTTP: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Error obteniendo cabeceras HTTP");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int total_read = 0;
    int read_len = 0;

    do {
        read_len = esp_http_client_read(client, buffer + total_read, max_len - total_read - 1);
        if (read_len > 0) {
            total_read += read_len;
        }
    } while (read_len > 0 && total_read < max_len - 1);

    buffer[total_read] = '\0';
    ESP_LOGI(TAG, "HTTP GET completado (%d bytes)", total_read);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return (total_read > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ota_get_stored_version(char *out, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ota_info", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    size_t required = len;
    err = nvs_get_str(nvs, "last_version", out, &required);
    nvs_close(nvs);
    return err;
}

esp_err_t ota_check_for_update(void)
{
    time_t now = time(NULL);
    int random_val = esp_random();
    char manifest_url[256];

    snprintf(manifest_url, sizeof(manifest_url),
             "%s?ts=%ld&r=%d", MANIFEST_URL, (long)now, random_val);

    ESP_LOGI(TAG, "Comprobando manifest remoto...");

    char json[1024];
    if (http_get(manifest_url, json, sizeof(json)) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo descargar el manifest");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "Error al parsear JSON");
        return ESP_FAIL;
    }

    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *url = cJSON_GetObjectItem(root, "url");

    if (!cJSON_IsString(ver) || !cJSON_IsString(url)) {
        ESP_LOGE(TAG, "Campos faltantes en manifest");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    char new_version[32] = {0};
    strncpy(new_version, ver->valuestring, sizeof(new_version) - 1);

    char bin_url[256] = {0};
    strncpy(bin_url, url->valuestring, sizeof(bin_url) - 1);

    char local_version[32] = {0};
    if (ota_get_stored_version(local_version, sizeof(local_version)) != ESP_OK) {
        strcpy(local_version, "0.0.0");
    }

    ESP_LOGI(TAG, "Versión local: %s | Versión remota: %s", local_version, new_version);

    if (strcmp(new_version, local_version) == 0) {
        ESP_LOGI(TAG, "Firmware actualizado. No se requiere OTA.");
        cJSON_Delete(root);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Nueva versión detectada: %s", new_version);

    nvs_handle_t nvs;
    esp_err_t nvs_err = nvs_open("ota_info", NVS_READWRITE, &nvs);
    if (nvs_err == ESP_OK) {
        nvs_set_str(nvs, "pending_version", new_version);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "pending_version guardada: %s", new_version);
    }

    cJSON_Delete(root);

    esp_err_t res = https_ota(bin_url);

    if (res == ESP_OK) {
        ESP_LOGI(TAG, "OTA completada. Reinicie el dispositivo para aplicar.");
    } else {
        nvs_err = nvs_open("ota_info", NVS_READWRITE, &nvs);
        if (nvs_err == ESP_OK) {
            nvs_erase_key(nvs, "pending_version");
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGW(TAG, "OTA falló. Se mantiene versión: %s", local_version);
    }

    return res;
}

typedef struct {
    int hour;
    int minute;
} ota_sched_params_t;

static TaskHandle_t s_scheduler_task = NULL;

static void scheduler_task(void *arg)
{
    ota_sched_params_t *p = (ota_sched_params_t *)arg;
    const int hour = p ? p->hour : 3;
    const int minute = p ? p->minute : 0;
    if (p) free(p);

    ESP_LOGI(TAG, "Programador OTA configurado para %02d:%02d", hour, minute);

    while (1) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_hour == hour && timeinfo.tm_min == minute) {
            ESP_LOGI(TAG, "Ejecutando OTA programada");

            ota_check_for_update();

            nvs_handle_t nvs;
            if (nvs_open("ota_info", NVS_READONLY, &nvs) == ESP_OK) {
                char pending[32] = {0};
                size_t len = sizeof(pending);
                if (nvs_get_str(nvs, "pending_version", pending, &len) == ESP_OK) {
                    ESP_LOGI(TAG, "Aplicando pending_version: %s", pending);
                    nvs_close(nvs);
                    esp_restart();
                }
                nvs_close(nvs);
            }

            vTaskDelay(pdMS_TO_TICKS(60 * 60 * 1000));
        }

        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

esp_err_t ota_schedule_daily(int hour, int minute)
{
    if (s_scheduler_task) {
        ESP_LOGI(TAG, "Programador OTA ya está activo");
        return ESP_OK;
    }

    ota_sched_params_t *params = malloc(sizeof(*params));
    if (!params) return ESP_ERR_NO_MEM;

    params->hour = hour;
    params->minute = minute;

    if (xTaskCreate(&scheduler_task, "ota_scheduler", 4096, params, 5, &s_scheduler_task) != pdPASS) {
        free(params);
        ESP_LOGE(TAG, "Error creando tarea OTA programada");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ota_apply_pending_now(void)
{
    nvs_handle_t nvs;
    if (nvs_open("ota_info", NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    char pending[32] = {0};
    size_t len = sizeof(pending);
    esp_err_t e = nvs_get_str(nvs, "pending_version", pending, &len);
    nvs_close(nvs);

    if (e == ESP_OK) {
        ESP_LOGI(TAG, "Aplicando pending_version: %s", pending);
        esp_restart();
    }

    return ESP_ERR_NOT_FOUND;
}