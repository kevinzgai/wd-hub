#pragma once
#include "esp_err.h"
#include "mbedtls/ssl.h"

// Called immediately after the handshake, before any application reads.
esp_err_t muse_tls_reserve_rx(mbedtls_ssl_context *ssl);
