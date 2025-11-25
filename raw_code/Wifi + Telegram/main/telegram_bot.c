#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include <string.h>
#include <stdlib.h>
#include "cJSON.h"

static const char *TAG = "TELEGRAM_BOT";

/* --- CONFIGURACIÓN --- */
#define BOT_TOKEN "8142762168:AAHr8ENvOrUo0pe5Knk5qjXsqlgWssqTkiE"
#define CHAT_ID   "1822580184"
#define POLL_INTERVAL_MS 3000
#define TELEGRAM_API_URL "https://api.telegram.org"

/* --- VARIABLES GLOBALES --- */
static long last_update_id = 0;

/* --- PROTOTIPO --- */
static void telegram_bot_task(void *pvParameters);

/* --- FUNCIONES --- */
static void telegram_send_message(const char *chat_id, const char *text)
{
    char url[256];
    snprintf(url, sizeof(url), TELEGRAM_API_URL "/bot%s/sendMessage", BOT_TOKEN);

    char post_data[512];
    snprintf(post_data, sizeof(post_data),
             "{\"chat_id\":\"%s\", \"text\":\"%s\"}", chat_id, text);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,  // << usar bundle
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Mensaje enviado: %s", text);
    } else {
        ESP_LOGE(TAG, "Error enviando mensaje: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void handle_command(const char *chat_id, const char *text)
{
    if (strncmp(text, "/help", 5) == 0) {
        telegram_send_message(chat_id,
            "📘 Comandos disponibles:\n"
            "/status - Estado del dispositivo\n"
        );
    }
    else if (strncmp(text, "/status", 7) == 0) {
        telegram_send_message(chat_id, "✅ Dispositivo activo");
    }
    else {
        telegram_send_message(chat_id,
            "❓ Comando no reconocido. Escribe /help para ver los comandos disponibles.");
    }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->user_data) {
                char *user_buffer = (char *)evt->user_data;
                int current_len = strlen(user_buffer);
                if (current_len + evt->data_len < 8192) {
                    memcpy(user_buffer + current_len, evt->data, evt->data_len);
                    user_buffer[current_len + evt->data_len] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void telegram_get_updates(void)
{
    char url[512];
    snprintf(url, sizeof(url),
             TELEGRAM_API_URL "/bot%s/getUpdates?offset=%ld&timeout=2&limit=1",
             BOT_TOKEN, last_update_id + 1);

    static char local_response_buffer[8192] = {0};
    memset(local_response_buffer, 0, sizeof(local_response_buffer));

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,  // << usar bundle
        .timeout_ms = 8000,
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(local_response_buffer);
        if (root) {
            cJSON *result = cJSON_GetObjectItem(root, "result");
            if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
                cJSON *update = cJSON_GetArrayItem(result, 0);
                cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
                if (cJSON_IsNumber(update_id)) {
                    last_update_id = update_id->valueint;
                }

                cJSON *message = cJSON_GetObjectItem(update, "message");
                if (message) {
                    cJSON *chat = cJSON_GetObjectItem(message, "chat");
                    cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
                    cJSON *text = cJSON_GetObjectItem(message, "text");
                    if (chat_id && text) {
                        char chat_id_str[32];
                        snprintf(chat_id_str, sizeof(chat_id_str), "%f", chat_id->valuedouble);
                        handle_command(chat_id_str, text->valuestring);
                    }
                }
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGE(TAG, "Error al obtener actualizaciones: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void telegram_bot_task(void *pvParameters)
{
    while (1) {
        telegram_get_updates();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void telegram_bot_start(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_NONE);

    ESP_LOGI(TAG, "Lanzando tarea Telegram...");
    xTaskCreatePinnedToCore(
        telegram_bot_task,
        "telegram_bot_task",
        16384,
        NULL,
        4,
        NULL,
        1
    );
}
