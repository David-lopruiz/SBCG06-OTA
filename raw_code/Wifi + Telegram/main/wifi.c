#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_http_server.h"

#include "PaginaWeb.h"

static const char *TAG = "webserver_softap";

static void urldecode(char *dst, const char *src)
{
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' '; src++;
        } else *dst++ = *src++;
    }
    *dst = 0;
}

static void get_form_value(const char *buf, const char *key, char *out, size_t n)
{
    const char *p = strstr(buf, key);
    if (!p) { out[0]=0; return; }
    p += strlen(key);
    const char *q = strchr(p, '&');
    size_t len = q ? (size_t)(q - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len]=0;
}

static esp_err_t wifi_form_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, wifi_form_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* --- Helpers NVS: guardar / cargar credenciales --- */
static esp_err_t save_credentials_nvs(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "password", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load_credentials_nvs(char *ssid, size_t ssid_size, char *password, size_t pass_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t required = ssid_size;
    err = nvs_get_str(h, "ssid", ssid, &required);
    if (err != ESP_OK) { nvs_close(h); return err; }
    required = pass_size;
    err = nvs_get_str(h, "password", password, &required);
    nvs_close(h);
    return err;
}

/* --- Reemplaza wifi_post_handler para guardar en NVS --- */
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    const size_t MAX = 512;
    int len = req->content_len;
    if (len <= 0 || len > (int)MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) return ESP_FAIL;
    int r = httpd_req_recv(req, buf, len);
    if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv error"); return ESP_FAIL; }
    buf[r]=0;

    char ssid_raw[128]={0}, pass_raw[128]={0}, ssid[33]={0}, pass[65]={0};
    get_form_value(buf, "ssid=", ssid_raw, sizeof(ssid_raw));
    get_form_value(buf, "password=", pass_raw, sizeof(pass_raw));
    urldecode(ssid, ssid_raw);
    urldecode(pass, pass_raw);
    free(buf);

    if (strlen(ssid) == 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required"); return ESP_FAIL; }

    wifi_config_t cfg = {0};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid)-1);
    strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password)-1);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_config %s", esp_err_to_name(err)); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_config"); return ESP_FAIL; }
    esp_wifi_connect();

    /* Guardar en NVS */
    esp_err_t nvs_err = save_credentials_nvs(ssid, pass);
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudieron guardar credenciales en NVS: %s", esp_err_to_name(nvs_err));
    } else {
        ESP_LOGI(TAG, "Credenciales guardadas en NVS");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static void register_handlers(httpd_handle_t srv)
{
    httpd_uri_t u1 = { .uri="/", .method=HTTP_GET, .handler=wifi_form_handler };
    httpd_register_uri_handler(srv, &u1);
    httpd_uri_t u2 = { .uri="/wifi", .method=HTTP_POST, .handler=wifi_post_handler };
    httpd_register_uri_handler(srv, &u2);
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) == ESP_OK) register_handlers(srv);
    return srv;
}

static void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) memset(mac, 0, sizeof(mac));
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "ESP_AP_%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t apcfg = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
    strncpy((char*)apcfg.ap.ssid, ssid, sizeof(apcfg.ap.ssid)-1);
    apcfg.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "softAP %s started", ssid);
}

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void* arg, esp_event_base_t evb, int32_t id, void* data)
{
    if (evb==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP");
        esp_wifi_set_mode(WIFI_MODE_STA);
    } else if (evb==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
}

/* --- Reemplaza app_main para cargar credenciales al inicio --- */
void wifi_start_and_wait(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_softap();

    /* Intentar cargar credenciales guardadas y conectar */
    char ssid_saved[33] = {0};
    char pass_saved[65] = {0};
    if (load_credentials_nvs(ssid_saved, sizeof(ssid_saved), pass_saved, sizeof(pass_saved)) == ESP_OK && strlen(ssid_saved) > 0) {
        ESP_LOGI(TAG, "Encontradas credenciales en NVS, intentando conectar a '%s'", ssid_saved);
        wifi_config_t cfg = {0};
        strncpy((char*)cfg.sta.ssid, ssid_saved, sizeof(cfg.sta.ssid)-1);
        strncpy((char*)cfg.sta.password, pass_saved, sizeof(cfg.sta.password)-1);
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "No se pudo aplicar configuración STA desde NVS");
        }
    } else {
        ESP_LOGI(TAG, "No hay credenciales en NVS");
    }

    start_webserver();

    /* Esperar hasta que se obtenga IP (bloqueante) */
    ESP_LOGI(TAG, "Esperando conexión WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi conectado.");
}