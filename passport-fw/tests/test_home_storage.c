#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/apps/home/home_profile.c"

static esp_partition_t partition = { .size = 2 * 1024 * 1024 };
static unsigned mount_calls, format_calls, reads, commits, delays, transient_failures;
static uint8_t first_byte = 0xff;
static bool missing_partition, format_fails;
static esp_err_t mount_error = ESP_FAIL;
static char cards_order[16] = "0a1b";

esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *config) {
    ++mount_calls;
    assert(!strcmp(config->partition_label, "storage"));
    if (config->format_if_mount_failed) {
        ++format_calls;
        return format_fails ? ESP_FAIL : ESP_OK;
    }
    if (transient_failures) { --transient_failures; return ESP_FAIL; }
    return mount_error;
}
void vTaskDelay(unsigned ticks) { (void)ticks; ++delays; }

/* home_profile.c 的画廊路径会查询容量;本测试不触碰,但 MinGW 链接器不丢弃未引用段。 */
esp_err_t esp_spiffs_info(const char *label, size_t *total, size_t *used) {
    assert(!strcmp(label, "storage"));
    *total = partition.size;
    *used = 0;
    return ESP_OK;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label) {
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS);
    assert(!strcmp(label, "storage"));
    return missing_partition ? NULL : &partition;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t size) {
    // Classification is a header peek only; the whole partition is never scanned.
    assert(p == &partition && offset == 0 && size <= 32);
    ++reads;
    memset(out, 0xff, size);
    ((uint8_t *)out)[0] = first_byte;
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    assert(!strcmp(ns, NVS_NAMESPACE));
    *h = mode;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *n) {
    (void)h; (void)key; (void)out; (void)n;
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; ++commits; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    (void)h;
    assert(!strcmp(key, "cards_v1"));
    snprintf(cards_order, sizeof(cards_order), "%s", value);
    return ESP_OK;
}

static void reset(void) {
    s_storage_mounted = false;
    mount_calls = format_calls = reads = commits = delays = 0;
    strcpy(cards_order, "0a1b");
    s_cards_loaded = s_avatar_loaded = true;
}

int main(void) {
    // Blank, another firmware's image (Xiaozhi OTA slot) or arbitrary data: always format.
    const uint8_t heads[] = {0xff, 0xe9, 0x7f};
    for (size_t i = 0; i < sizeof(heads); ++i) {
        reset();
        first_byte = heads[i];
        const uint32_t version = s_version;
        assert(ensure_spiffs() == ESP_OK);
        // Two plain mount attempts before anything is erased.
        assert(mount_calls == 3 && format_calls == 1 && reads == 1 && delays == 1);
        assert(!strcmp(cards_order, "") && commits == 1);
        assert(!s_cards_loaded && !s_avatar_loaded && s_version == version + 1);
        assert(ensure_spiffs() == ESP_OK && mount_calls == 3);
    }

    // A missing partition or an allocation failure must not format anything.
    reset();
    missing_partition = true;
    assert(ensure_spiffs() == ESP_ERR_NOT_FOUND && format_calls == 0);
    missing_partition = false;
    reset();
    mount_error = ESP_ERR_NO_MEM;
    assert(ensure_spiffs() == ESP_ERR_NO_MEM && format_calls == 0 && reads == 0);
    mount_error = ESP_FAIL;

    // A failed format keeps the indices and retries on the next call.
    reset();
    format_fails = true;
    assert(ensure_spiffs() == ESP_FAIL && format_calls == 1);
    assert(!strcmp(cards_order, "0a1b") && commits == 0);
    format_fails = false;
    assert(ensure_spiffs() == ESP_OK && format_calls == 2);

    // One transient mount failure keeps avatars and cards.
    reset();
    mount_error = ESP_OK;
    transient_failures = 1;
    assert(ensure_spiffs() == ESP_OK && mount_calls == 2 && format_calls == 0 && reads == 0);
    assert(!strcmp(cards_order, "0a1b") && commits == 0);

    // A healthy filesystem mounts once and leaves the indices untouched.
    reset();
    mount_error = ESP_OK;
    assert(ensure_spiffs() == ESP_OK && mount_calls == 1 && format_calls == 0 && commits == 0);

    // Home skipped its avatar while the boot preparation ran: bump the version so it reloads.
    reset();
    s_storage_preparing = true;
    assert(!home_profile_has_avatar());
    s_storage_preparing = false;
    s_storage_mounted = false;
    const uint32_t version = s_version;
    home_profile_storage_prepare();
    assert(s_storage_mounted && s_version == version + 1 && !s_storage_preparing);
    puts("Storage mount: blank/foreign/firmware auto-format, missing partition, allocation failure, retry, deferred refresh: PASS");
}
