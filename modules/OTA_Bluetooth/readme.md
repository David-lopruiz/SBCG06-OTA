# OTA por Bluetooth (ESP32, SPP + FreeRTOS)

Este módulo permite actualizar el firmware del ESP32 mediante Bluetooth clásico usando el perfil SPP, gestionado con FreeRTOS (task dedicada y `EventGroup`).

## Arquitectura

### Ficheros principales

- `ota_bt_update.h`  
  API pública del módulo.
- `ota_bt_update.c`  
  Implementación del servidor SPP y lógica OTA.

### Componentes internos

- **Task OTA (`ota_bt_task`)**
  - Task de FreeRTOS que:
    - Espera eventos de:
      - `EVT_RX_DATA`: hay datos nuevos en el buffer RX.
      - `EVT_STOP_TASK`: se solicita parar el módulo.
    - Llama a `process_rx_buffer()` para procesar los comandos OTA.

- **EventGroup (`s_ota_events`)**
  - `EVT_RX_DATA`: lo setea el callback SPP cuando llegan datos.
  - `EVT_STOP_TASK`: se setea en `ota_bt_stop()` para terminar la task.

- **Buffer circular RX (`rx_buffer_t`)**
  - `rx_buffer_append()`: añade bytes nuevos al buffer (desde el callback SPP).
  - `rx_buffer_read()` / `rx_buffer_peek()`: lectura/peek desde la task.

- **Estado global (`ota_bt_state_t`)**
  - Maneja:
    - Conexión SPP (`spp_handle`, `spp_state`).
    - Estado OTA (`ota_state`).
    - Partición, handle OTA y contadores de bytes/chunks.

## Protocolo OTA sobre SPP

Se definen 3 comandos básicos enviados desde el cliente Bluetooth:

- `PROTO_START_OTA = 0x01`
  - Formato:  
    `0x01 | size[3:0]` (4 bytes de tamaño, big-endian)  
    Total: 5 bytes.
  - Acción:
    - Selecciona partición OTA (`esp_ota_get_next_update_partition`).
    - Llama a `esp_ota_begin`.
    - Cambia `ota_state` a `OTA_STATE_RECEIVING`.
    - Responde:
      - `0xAA` (`PROTO_ACK`) si OK.
      - `0xFF` (`PROTO_NAK`) si error.

- `PROTO_DATA_CHUNK = 0x02`
  - Formato:  
    `0x02 | len[1:0] | datos...`  
    Donde `len` es `uint16_t` big-endian, máximo `MAX_CHUNK_PAYLOAD`.
  - Acción:
    - Escribe el chunk en flash con `esp_ota_write`.
    - Actualiza bytes recibidos y contador de chunks.
    - Responde `ACK`/`NAK`.

- `PROTO_END_OTA = 0x03`
  - Formato:  
    `0x03`
  - Acción:
    - Llama a `esp_ota_end`.
    - Llama a `esp_ota_set_boot_partition`.
    - Responde `ACK`.
    - Espera 2 segundos y llama a `esp_restart()`.

## Flujo interno de datos

1. El cliente envía datos por SPP.
2. El callback `esp_spp_cb()` recibe `ESP_SPP_DATA_IND_EVT`:
   - Copia los datos a `ota_state.rx_buf` con `rx_buffer_append()`.
   - Lanza el evento `EVT_RX_DATA` en `s_ota_events`.

3. La task `ota_bt_task`:
   - Está bloqueada en `xEventGroupWaitBits`.
   - Cuando recibe `EVT_RX_DATA`:
     - Llama a `process_rx_buffer(ota_state.spp_handle)`.
     - `process_rx_buffer`:
       - Lee el primer byte (comando).
       - Según el comando:
         - `PROTO_START_OTA`: inicia OTA.
         - `PROTO_DATA_CHUNK`: escribe chunk.
         - `PROTO_END_OTA`: termina OTA y reinicia.

4. Si se solicita parada:
   - `ota_bt_stop()` setea `EVT_STOP_TASK`.
   - La task sale del bucle y se autodestruye (`vTaskDelete`).

## API pública

Declarada en `ota_bt_update.h`:

```c
esp_err_t ota_bt_init(const char *device_name);
esp_err_t ota_bt_deinit(void);
esp_err_t ota_bt_finish_update(void);
esp_err_t ota_bt_stop(void);
```

### `esp_err_t ota_bt_init(const char *device_name)`

Inicializa todo el módulo OTA por Bluetooth:

- Crea el `EventGroup` (si no existe).
- Crea la task `ota_bt_task`.
- Inicializa el controlador BT clásico:
  - `esp_bt_controller_init`, `esp_bt_controller_enable`.
  - `esp_bluedroid_init`, `esp_bluedroid_enable`.
- Registra callbacks:
  - GAP: `esp_bt_gap_cb`.
  - SPP: `esp_spp_cb`.
- Inicializa SPP (`esp_spp_enhanced_init`) y arranca el servidor:
  - `esp_spp_start_srv(...)`.
- Configura:
  - Nombre del dispositivo (`device_name` o `ESP32_OTA_SPP`).
  - PIN fijo `1234`.
  - Modo visible/conectable.

**Uso típico** (desde `app_main` u otra parte):

```c
ESP_ERROR_CHECK(ota_bt_init("ESP32_OTA_SPP"));
```

### `esp_err_t ota_bt_stop(void)`

Detiene solo el servicio Bluetooth OTA, dejando el resto de la aplicación corriendo:

- Señala a la task para terminar (`EVT_STOP_TASK`).
- Aborta OTA en curso (`esp_ota_abort`) si procede.
- Apaga la pila Bluetooth:
  - `esp_spp_deinit()`
  - `esp_bluedroid_disable()` / `esp_bluedroid_deinit()`
  - `esp_bt_controller_disable()` / `esp_bt_controller_deinit()`

**Ejemplo de uso:**

```c
// Parar Bluetooth OTA cuando ya no se necesite
ota_bt_stop();
```

### `esp_err_t ota_bt_deinit(void)`

Desinicializa completamente el módulo:

- Llama internamente a `ota_bt_stop()`.
- Libera el `EventGroup`.
- Deja el módulo listo para no usarse más (se podría volver a llamar a `ota_bt_init` si se desea reactivar).

### `esp_err_t ota_bt_finish_update(void)`

Función auxiliar (actualmente mínima):

- Devuelve error si no hay una OTA en curso (`ESP_ERR_INVALID_STATE`).
- Pensada para extender si se quiere finalizar OTA sin reiniciar automáticamente.

## Callbacks de Bluetooth

### GAP (`esp_bt_gap_cb`)

Maneja:

- Autenticación completada (`ESP_BT_GAP_AUTH_CMPL_EVT`).
- Petición de PIN (`ESP_BT_GAP_PIN_REQ_EVT`): responde con `1234`.
- Confirmación SSP (`ESP_BT_GAP_CFM_REQ_EVT`).
- Notificación de PIN (`ESP_BT_GAP_KEY_NOTIF_EVT`).

### SPP (`esp_spp_cb`)

Eventos importantes:

- `ESP_SPP_INIT_EVT`: SPP inicializado.
- `ESP_SPP_START_EVT`: servidor SPP arrancado, se guarda `spp_handle`.
- `ESP_SPP_SRV_OPEN_EVT`: cliente conectado, se resetea el estado OTA y el buffer RX.
- `ESP_SPP_CLOSE_EVT`: cliente desconectado, si había OTA en curso se aborta.
- `ESP_SPP_DATA_IND_EVT`: llegada de datos; se copia al buffer y se lanza `EVT_RX_DATA`.

## Integración básica

1. Añadir los ficheros al proyecto (en `main/`):
   - `ota_bt_update.c`
   - `ota_bt_update.h`

2. Incluir el header donde se vaya a usar:

```c
#include "ota_bt_update.h"
```

3. Inicializar al arrancar la aplicación:

```c
ota_bt_init("ESP32_OTA_SPP");
```

4. (Opcional) Parar Bluetooth cuando ya no sea necesario:

```c
ota_bt_stop();
```

5. (Opcional) Desinicializar completamente el módulo:

```c
ota_bt_deinit();
```

El resto de la aplicación puede seguir usando FreeRTOS normalmente (otras tasks, colas, etc.) mientras la task `ota_bt_task` se encarga en segundo plano de la lógica OTA por Bluetooth.