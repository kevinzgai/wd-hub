#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct TestHttp *esp_http_client_handle_t;
typedef struct {
    const char *url;
    int timeout_ms, buffer_size, buffer_size_tx;
    const char *user_agent;
    bool keep_alive_enable, disable_auto_redirect;
    int max_redirection_count;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
#ifdef __cplusplus
}
#endif
