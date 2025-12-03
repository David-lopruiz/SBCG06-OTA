#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "esp_bt_device.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "ota_bt_update.h"

#define TAG "ota_bt"
#define SPP_SERVER_NAME "ESP32_OTA_SPP"

// Protocolo
#define PROTO_START_OTA 0x01
#define PROTO_DATA_CHUNK 0x02
#define PROTO_END_OTA 0x03
#define PROTO_ACK 0xAA
#define PROTO_NAK 0xFF

// Estados SPP
#define SPP_CONN_STATE_DISCONNECTED 0
#define SPP_CONN_STATE_CONNECTING 1
#define SPP_CONN_STATE_CONNECTED 2

// Estados OTA
#define OTA_STATE_IDLE 0
#define OTA_STATE_STARTED 1
#define OTA_STATE_RECEIVING 2
#define OTA_STATE_ENDING 3

// Buffer para recepción SPP (debe ser mayor que MTU SPP)
#define RX_BUFFER_SIZE 4096
#define MAX_CHUNK_PAYLOAD 1021

// Event group bits
#define EVT_RX_DATA   (1 << 0)
#define EVT_STOP_TASK (1 << 1)

typedef struct {
    uint8_t buffer[RX_BUFFER_SIZE];
    size_t head;     // Próximo byte a escribir
    size_t tail;     // Próximo byte a leer
    size_t count;    // Bytes disponibles
} rx_buffer_t;

typedef struct {
    uint32_t spp_handle;
    uint8_t spp_state;
    uint8_t ota_state;
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition;
    size_t bytes_received;
    size_t expected_size;
    uint32_t start_time;
    uint32_t chunk_count;
    rx_buffer_t rx_buf;
} ota_bt_state_t;

static ota_bt_state_t ota_state = {
    .spp_handle = 0,
    .spp_state = SPP_CONN_STATE_DISCONNECTED,
    .ota_state = OTA_STATE_IDLE,
    .bytes_received = 0,
    .expected_size = 0,
    .chunk_count = 0,
    .rx_buf = {}
};

// Task y eventos
static TaskHandle_t s_ota_task_handle = NULL;
static EventGroupHandle_t s_ota_events = NULL;

/**
 * @brief Agregar bytes al buffer circular de recepción
 */
static void rx_buffer_append(rx_buffer_t *buf, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        buf->buffer[buf->head] = data[i];
        buf->head = (buf->head + 1) % RX_BUFFER_SIZE;
        if (buf->count < RX_BUFFER_SIZE) {
            buf->count++;
        } else {
            // Buffer lleno, descartar datos antiguos (emergencia)
            buf->tail = (buf->tail + 1) % RX_BUFFER_SIZE;
            ESP_LOGW(TAG, "RX buffer overflow!");
        }
    }
}

/**
 * @brief Leer bytes del buffer circular
 */
static size_t rx_buffer_read(rx_buffer_t *buf, uint8_t *out, size_t len)
{
    size_t to_read = (len < buf->count) ? len : buf->count;
    
    for (size_t i = 0; i < to_read; i++) {
        out[i] = buf->buffer[buf->tail];
        buf->tail = (buf->tail + 1) % RX_BUFFER_SIZE;
        buf->count--;
    }
    
    return to_read;
}

/**
 * @brief Peek (leer sin consumir) del buffer
 */
static size_t rx_buffer_peek(rx_buffer_t *buf, uint8_t *out, size_t len)
{
    size_t to_read = (len < buf->count) ? len : buf->count;
    size_t pos = buf->tail;
    
    for (size_t i = 0; i < to_read; i++) {
        out[i] = buf->buffer[pos];
        pos = (pos + 1) % RX_BUFFER_SIZE;
    }
    
    return to_read;
}

/**
 * @brief Procesar paquetes del buffer
 */
static void process_rx_buffer(uint32_t handle)
{
    uint8_t response;
    rx_buffer_t *buf = &ota_state.rx_buf;
    
    while (buf->count > 0) {
        // Peek al primer byte (comando)
        uint8_t cmd;
        if (rx_buffer_peek(buf, &cmd, 1) < 1) break;
        
        // ========== START_OTA ==========
        if (cmd == PROTO_START_OTA) {
            if (buf->count < 5) {
                break;  // Esperar más datos
            }
            
            uint8_t header[5];
            rx_buffer_read(buf, header, 5);
            
            if (ota_state.ota_state != OTA_STATE_IDLE) {
                ESP_LOGW(TAG, "START_OTA rechazado (estado: %d)", ota_state.ota_state);
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            size_t size = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
            ESP_LOGI(TAG, "START_OTA: %zu bytes", size);
            
            ota_state.update_partition = esp_ota_get_next_update_partition(NULL);
            if (ota_state.update_partition == NULL) {
                ESP_LOGE(TAG, "Partición OTA no disponible");
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            esp_err_t err = esp_ota_begin(ota_state.update_partition, OTA_SIZE_UNKNOWN, &ota_state.ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin falló: %s", esp_err_to_name(err));
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            ota_state.ota_state = OTA_STATE_RECEIVING;
            ota_state.bytes_received = 0;
            ota_state.expected_size = size;
            ota_state.chunk_count = 0;
            ota_state.start_time = xTaskGetTickCount();
            
            response = PROTO_ACK;
            esp_spp_write(handle, 1, &response);
            ESP_LOGI(TAG, "OTA iniciada. Esperando %zu bytes", size);
        }
        // ========== DATA_CHUNK ==========
        else if (cmd == PROTO_DATA_CHUNK) {
            if (buf->count < 3) {
                break;  // Esperar más datos
            }
            
            uint8_t header[3];
            rx_buffer_peek(buf, header, 3);
            
            uint16_t chunk_len = (header[1] << 8) | header[2];
            
            if (chunk_len > MAX_CHUNK_PAYLOAD) {
                ESP_LOGE(TAG, "Longitud inválida: %u", chunk_len);
                rx_buffer_read(buf, header, 3);  // Descartar
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            if (buf->count < (3 + chunk_len)) {
                break;  // Esperar más datos
            }
            
            if (ota_state.ota_state != OTA_STATE_RECEIVING) {
                ESP_LOGW(TAG, "DATA_CHUNK rechazado (estado: %d)", ota_state.ota_state);
                rx_buffer_read(buf, header, 3 + chunk_len);  // Descartar
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            rx_buffer_read(buf, header, 3);
            
            uint8_t *chunk_data = (uint8_t *)malloc(chunk_len);
            if (chunk_data == NULL) {
                ESP_LOGE(TAG, "No hay memoria para chunk");
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            size_t read = rx_buffer_read(buf, chunk_data, chunk_len);
            if (read != chunk_len) {
                ESP_LOGE(TAG, "Lectura incompleta: %zu/%u", read, chunk_len);
                free(chunk_data);
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            esp_err_t err = esp_ota_write(ota_state.ota_handle, chunk_data, chunk_len);
            free(chunk_data);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write falló: %s", esp_err_to_name(err));
                esp_ota_abort(ota_state.ota_handle);
                ota_state.ota_state = OTA_STATE_IDLE;
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            ota_state.bytes_received += chunk_len;
            ota_state.chunk_count++;
            response = PROTO_ACK;
            esp_spp_write(handle, 1, &response);
        }
        // ========== END_OTA ==========
        else if (cmd == PROTO_END_OTA) {
            if (buf->count < 1) {
                break;
            }
            
            uint8_t end_byte;
            rx_buffer_read(buf, &end_byte, 1);
            
            if (ota_state.ota_state != OTA_STATE_RECEIVING) {
                ESP_LOGW(TAG, "END_OTA rechazado (estado: %d)", ota_state.ota_state);
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            ESP_LOGI(TAG, "END_OTA recibido");
            ota_state.ota_state = OTA_STATE_ENDING;
            
            if (ota_state.bytes_received != ota_state.expected_size) {
                ESP_LOGW(TAG, "Tamaño mismatch: %zu recibidos vs %zu esperados",
                    ota_state.bytes_received, ota_state.expected_size);
            }
            
            uint32_t elapsed_ms = (xTaskGetTickCount() - ota_state.start_time) * portTICK_PERIOD_MS;
            float elapsed_s = elapsed_ms / 1000.0;
            float speed_mbps = (ota_state.bytes_received / (float)(elapsed_ms ? elapsed_ms : 1)) * 1000.0f / (1024.0f * 1024.0f);
            
            ESP_LOGI(TAG, "OTA finalizada:");
            ESP_LOGI(TAG, "   - Bytes: %zu", ota_state.bytes_received);
            ESP_LOGI(TAG, "   - Chunks: %lu", ota_state.chunk_count);
            ESP_LOGI(TAG, "   - Tiempo: %.2f s", elapsed_s);
            ESP_LOGI(TAG, "   - Velocidad: %.2f MB/s", speed_mbps);
            
            esp_err_t err = esp_ota_end(ota_state.ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end falló: %s", esp_err_to_name(err));
                ota_state.ota_state = OTA_STATE_IDLE;
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            err = esp_ota_set_boot_partition(ota_state.update_partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition falló: %s", esp_err_to_name(err));
                ota_state.ota_state = OTA_STATE_IDLE;
                response = PROTO_NAK;
                esp_spp_write(handle, 1, &response);
                continue;
            }
            
            response = PROTO_ACK;
            esp_spp_write(handle, 1, &response);
            ESP_LOGI(TAG, "OTA confirmada. Reiniciando en 2s...");
            ota_state.ota_state = OTA_STATE_IDLE;
            
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        else {
            uint8_t bad_byte;
            rx_buffer_read(buf, &bad_byte, 1);
            ESP_LOGW(TAG, "Byte desconocido descartado: 0x%02X", bad_byte);
        }
    }
}

/**
 * @brief Task que procesa el buffer RX y gestiona OTA
 */
static void ota_bt_task(void *arg)
{
    ESP_LOGI(TAG, "OTA BT task iniciada");
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_ota_events,
            EVT_RX_DATA | EVT_STOP_TASK,
            pdTRUE,      // clear on exit
            pdFALSE,     // wait for any bit
            portMAX_DELAY
        );

        if (bits & EVT_STOP_TASK) {
            ESP_LOGI(TAG, "OTA BT task detenida");
            break;
        }

        if (bits & EVT_RX_DATA) {
            if (ota_state.spp_handle != 0) {
                process_rx_buffer(ota_state.spp_handle);
            }
        }
    }

    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief GAP callback
 */
static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "[GAP] Autenticación completada");
        } else {
            ESP_LOGE(TAG, "[GAP] Autenticación fallida: %d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(TAG, "[GAP] PIN requerido");
        esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "[GAP] SSP confirmación");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "[GAP] PIN: %06" PRIu32, param->key_notif.passkey);
        break;

    default:
        break;
    }
}

/**
 * @brief SPP callback - solo rellena buffer y notifica task
 */
static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "[SPP] Inicializado");
        break;

    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(TAG, "[SPP] Desconectado");
        ota_state.spp_state = SPP_CONN_STATE_DISCONNECTED;
        if (ota_state.ota_state != OTA_STATE_IDLE) {
            ESP_LOGW(TAG, "Abortando OTA: conexión cerrada");
            esp_ota_abort(ota_state.ota_handle);
            ota_state.ota_state = OTA_STATE_IDLE;
        }
        memset(&ota_state.rx_buf, 0, sizeof(rx_buffer_t));
        break;

    case ESP_SPP_START_EVT:
        ESP_LOGI(TAG, "[SPP] Servidor iniciado");
        ota_state.spp_handle = param->start.handle;
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(TAG, "[SPP] Cliente conectado");
        ota_state.spp_handle = param->srv_open.handle;
        ota_state.spp_state = SPP_CONN_STATE_CONNECTED;
        ota_state.ota_state = OTA_STATE_IDLE;
        memset(&ota_state.rx_buf, 0, sizeof(rx_buffer_t));
        break;

    case ESP_SPP_DATA_IND_EVT: {
        uint8_t *data = param->data_ind.data;
        uint16_t len = param->data_ind.len;
        if (len == 0) break;

        rx_buffer_append(&ota_state.rx_buf, data, len);
        ESP_LOGD(TAG, "SPP RX: %u bytes (buffer: %zu/%d)", len, ota_state.rx_buf.count, RX_BUFFER_SIZE);

        if (s_ota_events) {
            xEventGroupSetBits(s_ota_events, EVT_RX_DATA);
        }
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

esp_err_t ota_bt_init(const char *device_name)
{
    if (!s_ota_events) {
        s_ota_events = xEventGroupCreate();
        if (!s_ota_events) {
            ESP_LOGE(TAG, "No se pudo crear event group");
            return ESP_FAIL;
        }
    }

    if (!s_ota_task_handle) {
        BaseType_t res = xTaskCreate(
            ota_bt_task,
            "ota_bt_task",
            4096,
            NULL,
            5,
            &s_ota_task_handle
        );
        if (res != pdPASS) {
            ESP_LOGE(TAG, "No se pudo crear task OTA BT");
            vEventGroupDelete(s_ota_events);
            s_ota_events = NULL;
            return ESP_FAIL;
        }
    }

    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;

    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = false,
        .tx_buffer_size = 0
    };

    ESP_ERROR_CHECK(esp_spp_enhanced_init(&spp_cfg));

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    ESP_ERROR_CHECK(esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0,
        device_name ? device_name : SPP_SERVER_NAME));

    esp_bt_gap_set_device_name(device_name ? device_name : SPP_SERVER_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ESP_LOGI(TAG, "Bluetooth OTA inicializado - PIN: 1234");
    ESP_LOGI(TAG, "RX Buffer: %d bytes (max %d por chunk)", RX_BUFFER_SIZE, MAX_CHUNK_PAYLOAD);

    return ESP_OK;
}

esp_err_t ota_bt_stop(void)
{
    // Señalamos a la task que termine
    if (s_ota_events && s_ota_task_handle) {
        xEventGroupSetBits(s_ota_events, EVT_STOP_TASK);
        // Esperar un poco a que muera la task (opcional)
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Abortamos OTA en curso si la hay
    if (ota_state.ota_state != OTA_STATE_IDLE) {
        esp_ota_abort(ota_state.ota_handle);
        ota_state.ota_state = OTA_STATE_IDLE;
    }

    // Apagar BT
    esp_spp_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    ESP_LOGI(TAG, "Bluetooth OTA detenido");

    return ESP_OK;
}

esp_err_t ota_bt_deinit(void)
{
    // Deinit completo: detener BT + borrar recursos de FreeRTOS
    ota_bt_stop();

    if (s_ota_events) {
        vEventGroupDelete(s_ota_events);
        s_ota_events = NULL;
    }

    ESP_LOGI(TAG, "Bluetooth OTA desinicializado");

    return ESP_OK;
}

esp_err_t ota_bt_finish_update(void)
{
    if (ota_state.ota_state == OTA_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}
