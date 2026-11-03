#include <assert.h>
#include "proxy_route.h"

int main(void) {
    assert(muse_proxy_ipv4_ok("172.20.10.1") && muse_proxy_ipv4_ok("192.168.43.1"));
    assert(!muse_proxy_ipv4_ok("") && !muse_proxy_ipv4_ok("0.0.0.0") && !muse_proxy_ipv4_ok("127.0.0.1"));
    assert(!muse_proxy_ipv4_ok("192.168.1.255") && !muse_proxy_ipv4_ok("224.0.0.1") && !muse_proxy_ipv4_ok("phone.local"));
    return 0;
}
