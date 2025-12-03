#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ccs811.h"

void app_main(void)
{
    ccs811_data_t last_sample = {0};

    // Iniciar módulo CCS811 (crea la tarea internamente)
    if (ccs811_start(&last_sample) != 0) {
        printf("Error iniciando CCS811\n");
        return;
    }

    while (1) {
        ccs811_data_t cur;

        if (ccs811_read_safe(&cur, 100) == 0) {
            printf("eCO2 = %u ppm | TVOC = %u ppb | STATUS=0x%02X\n",
                   cur.eco2, cur.tvoc, cur.status);
        } else {
            printf("No se pudo leer datos del CCS811\n");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        // Ejemplo de parada opcional:
        // después de un rato podrías llamar a ccs811_stop();
    }
}