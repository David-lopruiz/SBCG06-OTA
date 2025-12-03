# Sensor de Calidad de Aire CCS811 (eCO₂ y TVOC) con ESP32

Este proyecto integra el sensor CCS811 (como el **Pmod AQS**) con un ESP32 usando **ESP-IDF** y **FreeRTOS**.  
Permite leer:

- **eCO₂ (ppm)** – CO₂ equivalente en partes por millón  
- **TVOC (ppb)** – Compuestos Orgánicos Volátiles totales en partes por mil millones  

---

## 1. Conexiones de Hardware

Conecta el módulo CCS811 al ESP32 por **I²C**:

- `INT` → **Libre** (no usado en este ejemplo)
- `W/R` → **GND**
- `SCL` → **GPIO22** (SCL)
- `SDA` → **GPIO21** (SDA)
- `GND` → **GND**
- `VCC` → **3.3V / VCC** (según tu placa, normalmente 3.3 V)

Dirección I²C usada por el código:

```c
#define CCS811_ADDR 0x5B
```

---

## 2. Archivos Principales

- `SensorGas.h` / `SensorGas.c`  
  API de alto nivel para el sensor CCS811:
  - Inicialización
  - Tarea periódica de lectura
  - Lectura segura desde otras tareas
  - Parada y liberación de recursos

- `main.c`  
  Ejemplo mínimo de cómo usar la API del sensor.

---

## 3. API del Sensor

Declarada en `SensorGas.h`:

```c
typedef struct {
    uint16_t eco2;   // eCO₂ en ppm
    uint16_t tvoc;   // TVOC en ppb
    uint8_t  status; // Registro de estado del CCS811
} ccs811_data_t;

/**
 * Inicia el CCS811 y lanza la tarea de lectura periódica.
 * @param shared_data Puntero a estructura donde se guardarán las últimas lecturas.
 * @return 0 en éxito, <0 en error.
 */
int ccs811_start(ccs811_data_t *shared_data);

/**
 * Copia de forma segura la última lectura a 'out'.
 * @param out Estructura destino.
 * @param timeout_ms Timeout para el mutex (en ms).
 * @return 0 en éxito, <0 en error.
 */
int ccs811_read_safe(ccs811_data_t *out, TickType_t timeout_ms);

/**
 * Detiene la tarea del CCS811 y libera recursos.
 * @return 0 en éxito, <0 en error.
 */
int ccs811_stop(void);
```

### 3.1. `ccs811_start`

- Inicializa el bus I²C (GPIO21/22).
- Envia los comandos de arranque al CCS811.
- Configura el modo de medición a **1 Hz**.
- Crea una tarea FreeRTOS que:
  - Lee eCO₂ y TVOC cada segundo.
  - Guarda el último valor leído en `shared_data` protegido por un mutex.
  - Muestra las lecturas por log (`ESP_LOGI`).

### 3.2. `ccs811_read_safe`

- Accede a la última muestra del sensor de forma **thread-safe**:
  - Bloquea el mutex interno.
  - Copia la lectura a la estructura `out`.
  - Libera el mutex.
- `timeout_ms` define el tiempo máximo para esperar el mutex.

### 3.3. `ccs811_stop`

- Señala a la tarea de lectura que debe terminar.
- Elimina la tarea y el mutex.
- Pone el CCS811 en modo **Idle** (sin mediciones).

---

## 4. Uso en `app_main`

Ejemplo básico (ya incluido en `main.c`):

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "SensorGas.h"   // o "ccs811.h" según el nombre real del header

void app_main(void)
{
    ccs811_data_t last_sample = {0};

    // Iniciar el módulo CCS811 (crea internamente la tarea de lectura)
    if (ccs811_start(&last_sample) != 0) {
        printf("Error iniciando CCS811\n");
        return;
    }

    while (1) {
        ccs811_data_t cur;

        // Leer la última muestra disponible de forma segura
        if (ccs811_read_safe(&cur, 100) == 0) {
            printf("eCO2 = %u ppm | TVOC = %u ppb | STATUS=0x%02X\n",
                   cur.eco2, cur.tvoc, cur.status);
        } else {
            printf("No se pudo leer datos del CCS811\n");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        // Si se desea parar el sensor después de un tiempo:
        // ccs811_stop();
    }
}
```

---

## 5. Interpretación de las Lecturas

### 5.1. eCO₂ (ppm)

| eCO₂ (ppm)    | Interpretación                                                        | Calidad del aire |
| ------------- | --------------------------------------------------------------------- | ---------------- |
| **400–600**   | Aire limpio (exterior o bien ventilado)                              | Excelente        |
| **600–1000**  | Ocupación normal en interior                                         | Buena            |
| **1000–1500** | Mal ventilado                                                        | Regular          |
| **1500–2500** | Saturación, acumulación de CO₂ equivalente                           | Mala             |
| **2500–5000** | Muy mala ventilación                                                 | Muy mala         |
| **>5000**     | Exposición peligrosa (el sensor llega hasta ~8192 ppm aprox.)        | Crítico          |

### 5.2. TVOC (ppb)

| TVOC (ppb)    | Interpretación                     | Calidad del aire |
| ------------- | ---------------------------------- | ---------------- |
| **0–50**      | Aire muy limpio                    | Excelente        |
| **50–200**    | Actividad humana normal            | Buena            |
| **200–600**   | Productos químicos, perfumes, cocina| Regular         |
| **600–2000**  | Contaminado (limpiadores, humo)    | Mala             |
| **>2000**     | Muy contaminado                    | Muy mala         |

---

## 6. Compilación (ESP-IDF)

En la carpeta del proyecto (`Tests`):

```bash
idf.py set-target esp32
idf.py menuconfig      # Opcional, si quieres ajustar algo
idf.py build
idf.py flash monitor
```

Asegúrate de que en `CMakeLists.txt` esté registrado el componente:

```cmake
idf_component_register(SRCS "SensorGas.c" "main.c"
                    INCLUDE_DIRS ".")
```

(Ajusta el nombre del archivo fuente del sensor según tu proyecto real.)

---

## 7. Notas

- Alimenta siempre el CCS811 con el voltaje recomendado (normalmente 3.3 V).
- Evita colocar el sensor muy cerca de fuentes de calor intensas.
- Deja al sensor unos minutos para estabilizar la lectura tras encenderlo.