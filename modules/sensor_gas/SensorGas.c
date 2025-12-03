#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "ccs811.h"

#define TAG "CCS811"

// Pines I2C (los mismos que usas en main.c)
#define SDA_PIN 21
#define SCL_PIN 22
#define I2C_FREQ_HZ 100000

// Contexto de la tarea (similar a sensoramb.c)
typedef struct {
    ccs811_data_t *shared_data;   // puntero proporcionado por el llamador
    SemaphoreHandle_t lock;       // mutex para proteger shared_data
    TaskHandle_t task;            // handle de la task creada
    volatile bool stop_requested; // señal para parar la task
} ccs811_ctx_t;

static ccs811_ctx_t *global_ctx = NULL;

/******************* Funciones internas I2C + driver CCS811 *******************/

// Init I2C (driver legacy, como ya usabas)
static void ccs811_i2c_init(void)
{
    static bool initialized = false;
    if (initialized) return;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = SCL_PIN,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0));
    initialized = true;
}

// Lee un registro de 1 byte
static uint8_t ccs811_read8(uint8_t reg)
{
    uint8_t value = 0;
    i2c_master_write_read_device(I2C_NUM_0, CCS811_ADDR,
                                 &reg, 1, &value, 1,
                                 1000 / portTICK_PERIOD_MS);
    return value;
}

// Escribe n bytes a partir de un registro
static esp_err_t ccs811_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[len + 1];
    buf[0] = reg;
    if (len > 0 && data != NULL) {
        memcpy(&buf[1], data, len);
    }

    return i2c_master_write_to_device(I2C_NUM_0, CCS811_ADDR,
                                      buf, len + 1,
                                      1000 / portTICK_PERIOD_MS);
}

static uint8_t ccs811_read_status_raw(void)
{
    return ccs811_read8(0x00);  // STATUS
}

static void ccs811_set_mode_raw(uint8_t mode)
{
    // mode: 0x00 = Idle, 0x10 = 1s, 0x20 = 10s, etc.
    uint8_t m = mode;
    ccs811_write(0x01, &m, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void ccs811_init_raw(void)
{
    ESP_LOGI(TAG, "Inicializando CCS811...");

    // APP_START
    uint8_t cmd = 0xF4;
    i2c_master_write_to_device(I2C_NUM_0, CCS811_ADDR,
                               &cmd, 1,
                               1000 / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Modo 1 medición (0x10, 1 Hz)
    ccs811_set_mode_raw(0x10);
}

// Lee eCO2 y TVOC
static ccs811_data_t ccs811_read_measurement(void)
{
    ccs811_data_t result = {0};
    uint8_t reg = 0x02;  // ALG_RESULT_DATA
    uint8_t buf[4] = {0};

    i2c_master_write_read_device(I2C_NUM_0, CCS811_ADDR,
                                 &reg, 1, buf, 4,
                                 1000 / portTICK_PERIOD_MS);

    result.eco2 = (buf[0] << 8) | buf[1];
    result.tvoc = (buf[2] << 8) | buf[3];
    result.status = ccs811_read_status_raw();
    return result;
}

/******************* Tarea FreeRTOS *******************/

static void ccs811_task(void *arg)
{
    ccs811_ctx_t *ctx = (ccs811_ctx_t *)arg;

    if (!ctx || !ctx->shared_data) {
        ESP_LOGE(TAG, "ccs811_task: contexto inválido");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (ctx->stop_requested) {
            break;
        }

        ccs811_data_t sample = ccs811_read_measurement();

        // Copiar al buffer compartido protegido por mutex
        if (xSemaphoreTake(ctx->lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            *(ctx->shared_data) = sample;
            xSemaphoreGive(ctx->lock);
        }

        ESP_LOGI(TAG, "eCO2: %d ppm | TVOC: %d ppb | STATUS: 0x%02X",
                 sample.eco2, sample.tvoc, sample.status);

        vTaskDelay(pdMS_TO_TICKS(1000));  // 1 lectura por segundo (coherente con modo 1)
    }

    ESP_LOGI(TAG, "ccs811_task: finalizando");
    vTaskDelete(NULL);
}

/******************* API pública *******************/

int ccs811_start(ccs811_data_t *shared_data)
{
    if (!shared_data) return -1;
    if (global_ctx != NULL) return -2; // ya iniciado

    ccs811_i2c_init();
    ccs811_init_raw();

    ccs811_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -3;

    ctx->shared_data = shared_data;
    ctx->lock = xSemaphoreCreateMutex();
    if (!ctx->lock) {
        free(ctx);
        return -4;
    }
    ctx->stop_requested = false;
    ctx->task = NULL;

    global_ctx = ctx;

    BaseType_t ok = xTaskCreate(ccs811_task,
                                "ccs811_task",
                                4096,
                                ctx,
                                tskIDLE_PRIORITY + 5,
                                &(ctx->task));
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Error creando task del CCS811");
        vSemaphoreDelete(ctx->lock);
        free(ctx);
        global_ctx = NULL;
        return -5;
    }

    return 0;
}

int ccs811_read_safe(ccs811_data_t *out, TickType_t timeout_ms)
{
    if (!out) return -1;
    if (!global_ctx) return -2;

    if (xSemaphoreTake(global_ctx->lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return -3;

    *out = *(global_ctx->shared_data);
    xSemaphoreGive(global_ctx->lock);
    return 0;
}

int ccs811_stop(void)
{
    if (!global_ctx) return -1;

    global_ctx->stop_requested = true;
    if (global_ctx->task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(200));   // margen para que salga del bucle
        vTaskDelete(global_ctx->task);    // por si sigue viva
    }

    if (global_ctx->lock) vSemaphoreDelete(global_ctx->lock);
    free(global_ctx);
    global_ctx = NULL;

    // Opcional: poner el sensor en Idle
    ccs811_set_mode_raw(0x00);

    return 0;
}