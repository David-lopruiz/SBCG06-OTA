#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* funciones exportadas de otros módulos */
extern void wifi_start_and_wait(void);
extern void telegram_bot_start(void);

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando WiFi y esperando conexión...");
    wifi_start_and_wait();

    ESP_LOGI(TAG, "Arrancando Telegram bot...");
    telegram_bot_start();

    /* El task principal puede eliminarse si no hace nada más */
    vTaskDelete(NULL);
}