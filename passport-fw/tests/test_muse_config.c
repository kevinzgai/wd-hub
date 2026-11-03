#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config_store.h"
#include "nvs.h"

typedef struct { char key[24], value[128]; } entry_t;
static entry_t durable[16], pending[16];
static bool opened, writable, fail_write, fail_commit;

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle) {
    assert(strcmp(ns, "passport_muse") == 0 && !opened);
    opened = true; writable = mode == NVS_READWRITE;
    memcpy(pending, durable, sizeof(pending)); *handle = 1; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 && opened); opened = false; }
static entry_t *find(entry_t *entries, const char *key) {
    for (unsigned i = 0; i < 16; ++i) if (!strcmp(entries[i].key, key)) return &entries[i];
    return NULL;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size) {
    assert(handle == 1 && opened);
    entry_t *e = find(durable, key); if (!e) return ESP_ERR_NVS_NOT_FOUND;
    size_t needed = strlen(e->value) + 1;
    if (!out) { *size = needed; return ESP_OK; }
    if (*size < needed) return ESP_ERR_INVALID_SIZE;
    memcpy(out, e->value, needed); *size = needed; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    assert(handle == 1 && opened && writable);
    if (fail_write) return ESP_FAIL;
    entry_t *e = find(pending, key);
    if (!e) for (unsigned i = 0; i < 16; ++i) if (!pending[i].key[0]) { e = &pending[i]; break; }
    assert(e && strlen(key) < sizeof(e->key) && strlen(value) < sizeof(e->value));
    strcpy(e->key, key); strcpy(e->value, value); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    assert(handle == 1 && opened && writable);
    entry_t *e = find(pending, key); if (!e) return ESP_ERR_NVS_NOT_FOUND;
    memset(e, 0, sizeof(*e)); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1 && opened && writable);
    if (fail_commit) return ESP_FAIL;
    memcpy(durable, pending, sizeof(durable)); return ESP_OK;
}
static void reboot(void) {
    assert(!opened); memset(pending, 0, sizeof(pending)); config_store_init();
}
static void expect(const char *key, const char *expected) {
    char value[128]; assert(config_get_str(key, value, sizeof(value)));
    assert(!strcmp(value, expected));
}
int main(void) {
    config_store_init(); char value[64] = "not empty";
    assert(!config_get_str("sdk_token", value, sizeof(value)) && !value[0]);
    assert(config_set_str("sdk_token", "mgst_test_fixture_only_12345"));
    assert(config_set_str("http_proxy", "192.168.1.20:7897"));
    assert(config_set_str("access_token", "fixture_access"));
    assert(config_set_str("refresh_token", "fixture_refresh"));
    assert(config_mark_setup_complete() && config_is_provisioned());
    reboot();
    expect("sdk_token", "mgst_test_fixture_only_12345");
    expect("http_proxy", "192.168.1.20:7897");
    assert(config_is_provisioned());
    fail_write = true; assert(!config_set_str("http_proxy", "192.168.1.20:8080"));
    fail_write = false; fail_commit = true;
    assert(!config_set_str("sdk_token", "mgst_new_fixture_only"));
    fail_commit = false; reboot();
    expect("sdk_token", "mgst_test_fixture_only_12345");
    expect("http_proxy", "192.168.1.20:7897");
    assert(config_set_str("http_proxy", "192.168.1.20:8080"));
    assert(config_is_provisioned());
    assert(config_clear_pairing() && !config_is_provisioned());
    reboot();
    expect("sdk_token", "mgst_test_fixture_only_12345");
    expect("http_proxy", "192.168.1.20:8080");
    assert(config_clear_setup());
    expect("sdk_token", "mgst_test_fixture_only_12345");
    assert(config_set_str("http_proxy", "")); reboot(); expect("http_proxy", "");
    puts("Muse NVS: reboot retention, pairing reset preservation and failed writes: PASS");
    return 0;
}
