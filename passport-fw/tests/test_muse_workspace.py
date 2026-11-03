"""Fault-inject production session allocation and cleanup under ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "components/passport_muse/sdk/noise_control.cpp").read_text()
limits = source[source.index("// On a board with the full UI"):source.index("static char s_node_id")]
setup = source[source.index("static session_result_t run_session("):source.index('    ESP_LOGI(TAG, "TLS connected')]
cleanup = source[source.index("cleanup:\n", source.index("static session_result_t run_session(")):
                 source.index("static void noise_ctrl_task(")]

stubs = r'''
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>
#include "bsp_audio_workspace.h"
extern unsigned char *app_workspace();
extern bool app_acquire();
extern bool app_release();
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define ESP_OK 0
#define NOISE_PORT 443
enum session_result_t { SESSION_FAILED };
enum noise_upgrade_result_t { NOISE_UPGRADE_OK };
using noise_ctrl_session_generation_t = unsigned;
using stack_monitor_t = int;
struct pending_json_body {};
struct identity_fetch { void *body; };
struct PsaCryptoBackend {};
struct ClientSession {
    static constexpr size_t kMaxOutboundWebSocketPayloadSize = 65536;
    explicit ClientSession(PsaCryptoBackend &) {}
};
struct esp_tls_t {};
struct esp_tls_cfg_t { int crt_bundle_attach; int timeout_ms; };
static int esp_crt_bundle_attach;
static const char *s_noise_host = "test.invalid";
static bool proxy, fail_tls;
static int calls, fail_at, tls_destroyed;
static std::set<void *> live;
static std::vector<size_t> sizes;
static void *test_malloc(size_t n) {
    sizes.push_back(n);
    if (++calls == fail_at) return nullptr;
    void *p = std::malloc(n);
    assert(p); live.insert(p); return p;
}
static void test_free(void *p) {
    if (!p) return;
    assert(live.erase(p) == 1); std::free(p);
}
static void *heap_caps_malloc(size_t, int) { return nullptr; }
static size_t heap_caps_get_free_size(int) { return 90000; }
static size_t heap_caps_get_largest_free_block(int) { return 60000; }
static void clear_pending_json_body(pending_json_body &) {}
static void notify_status(const char *) {}
static bool muse_proxy_enabled() { return proxy; }
static esp_tls_t tls_instance;
static esp_tls_t *esp_tls_init() { return fail_tls ? nullptr : &tls_instance; }
static int esp_tls_conn_new_sync(const char *, int, int, esp_tls_cfg_t *, esp_tls_t *) { return -1; }
static int muse_tls_connect_proxy(const char *, int, int, esp_tls_t **tls) {
    *tls = fail_tls ? nullptr : &tls_instance; return -1;
}
static void esp_tls_conn_destroy(esp_tls_t *tls) {
    assert(tls == &tls_instance); ++tls_destroyed;
#if CONFIG_IDF_TARGET_ESP32C3
    assert(audio_workspace::owner == audio_workspace::Owner::Muse);
#endif
}
#define malloc test_malloc
#define free test_free
'''

checks = r'''
#undef malloc
#undef free
int main() {
    using namespace audio_workspace;
    assert(app_workspace() == storage);
    assert(!acquire(Owner::None) && !release(Owner::None));
    assert(acquire(Owner::Radio));
    assert(!app_acquire() && !release(Owner::Muse));
#if SHARED_MUSE_WORKSPACE
    std::memset(storage, 0x5a, kBytes);
    run_session(nullptr);
    assert(calls == 0 && owner == Owner::Radio);
    for (auto byte : storage) assert(byte == 0x5a);
#endif
    assert(release(Owner::Radio));
    const int allocations = SHARED_MUSE_WORKSPACE ? 0 : RX_DECRYPT_IN_PLACE ? 5 : 6;
    for (proxy = false;; proxy = true) {
        for (int failed = 0; failed <= allocations; ++failed) {
            for (int no_tls = 0; no_tls <= 1; ++no_tls) {
                calls = 0; fail_at = failed; tls_destroyed = 0;
                sizes.clear(); fail_tls = no_tls;
                assert(run_session(nullptr) == SESSION_FAILED);
                assert(calls == allocations && live.empty() && owner == Owner::None);
                assert(tls_destroyed == (!failed && !no_tls));
#if SHARED_MUSE_WORKSPACE
                assert(sizes.empty());
                assert(muse::service_scratch() == aux() + 17 * 1024);
                assert(muse::envelope_scratch() + muse::kSendScratchBytes <= aux() + kAuxBytes);
#endif
                assert(app_acquire());
                assert(!acquire(Owner::Muse) && !release(Owner::Radio));
                assert(app_release());
            }
        }
        if (proxy) break;
    }
}
'''

with tempfile.TemporaryDirectory(prefix="passport-muse-workspace-") as temporary:
    directory = Path(temporary)
    code = directory / "session.cpp"
    code.write_text(stubs + limits + setup + "\n    goto cleanup;\n" + cleanup + checks)
    other = directory / "app.cpp"
    other.write_text('''#include "audio_workspace.h"
unsigned char *app_workspace() { return audio_workspace::storage; }
bool app_acquire() { return audio_workspace::acquire(audio_workspace::Owner::Xiaozhi); }
bool app_release() { return audio_workspace::release(audio_workspace::Owner::Xiaozhi); }
''')
    for c3, psram in ((1, 0), (0, 0), (0, 1)):
        executable = directory / f"workspace-{c3}-{psram}"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-g", "-O1",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                        "-Wno-unused-parameter", "-Wno-unused-function",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-DCONFIG_MUSE_ENABLED=1", "-DCONFIG_HOMEHUB_TUNNEL=0",
                        "-DCONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV=0",
                        f"-DCONFIG_IDF_TARGET_ESP32C3={c3}", f"-DCONFIG_SPIRAM={psram}",
                        "-Icomponents/bsp/include", "-Imain", str(code), str(other),
                        "-o", str(executable)], cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
print("Muse workspace allocation/cleanup and cross-component ownership: PASS")
