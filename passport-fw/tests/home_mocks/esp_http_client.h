#pragma once
#include <stdbool.h>
#include "esp_err.h"
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef struct TestHttp *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADER_SENT } esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    void *data;
    int data_len;
    void *user_data;
    esp_http_client_handle_t client;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    int timeout_ms, buffer_size, buffer_size_tx;
    esp_err_t (*crt_bundle_attach)(void *);
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    bool is_async, disable_auto_redirect;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
int esp_http_client_get_status_code(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
