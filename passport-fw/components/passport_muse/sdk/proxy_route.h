/* IPv4 validation shared by the Muse proxy configuration and host tests. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_PROXY_HOST_MAX 16

/* Literal unicast IPv4 a LAN proxy could listen on. */
bool muse_proxy_ipv4_ok(const char *host);

#ifdef __cplusplus
}
#endif
