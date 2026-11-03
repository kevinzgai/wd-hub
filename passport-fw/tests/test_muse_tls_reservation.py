"""Exercise the production pre-read TLS reservation gate and cleanup call."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'components/passport_muse/sdk/noise_control.cpp').read_text()
begin = source.index('    // Reserve the maximum RX record')
end = source.index('    // Passport OS can leave', begin)
gate = source[begin:end]
cleanup = '    if (tls) esp_tls_conn_destroy(tls);'
assert cleanup in source[end:]
assert source.index('esp_tls_conn_new_sync', source.index('static session_result_t run_session')) < begin
assert end < source.index('upgrade = ws_upgrade(tls)', end)
code = r'''
#include <cassert>
#include <cstring>
struct mbedtls_ssl_context {};
static mbedtls_ssl_context ssl;
static bool context_missing, fail_reserve;
static unsigned reservations, upgrades, destroys;
static const char *status;
static void esp_tls_conn_destroy(int *p) { assert(p); ++destroys; }
static void *esp_tls_get_ssl_context(int *) { return context_missing ? nullptr : &ssl; }
static int muse_tls_reserve_rx(mbedtls_ssl_context *p) {
    assert(p==&ssl); ++reservations; return fail_reserve ? -1 : 0;
}
static void notify_status(const char *s) { status=s; }
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static bool session() {
    int connection=1; int *tls=&connection;
    bool ready=false;
''' + gate + r'''
    ++upgrades;
    ready=true;
cleanup:
''' + cleanup + r'''
    return ready;
}
int main() {
    context_missing=true;
    assert(!session() && reservations==0 && upgrades==0 && destroys==1);
    assert(!strcmp(status,"ws_memory_failed"));
    context_missing=false; fail_reserve=true;
    assert(!session() && reservations==1 && upgrades==0 && destroys==2);
    assert(!strcmp(status,"ws_memory_failed"));
    fail_reserve=false;
    assert(session() && reservations==2 && upgrades==1 && destroys==3);
    assert(session() && reservations==3 && upgrades==2 && destroys==4);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'reservation.cpp'
    path.write_text(code)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
print('Muse TLS reservation before reads, cleanup call and fresh reconnect reservation: PASS')
