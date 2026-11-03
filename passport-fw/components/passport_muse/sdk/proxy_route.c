#include "proxy_route.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#endif

bool muse_proxy_ipv4_ok(const char *host)
{
    struct in_addr address;
    if (!host || strlen(host) >= MUSE_PROXY_HOST_MAX || inet_pton(AF_INET, host, &address) != 1) return false;
    uint32_t ip = ntohl(address.s_addr);
    return ip != 0 && (ip >> 24) != 127 && (ip >> 24) < 224 && (ip & 255) != 255;
}

