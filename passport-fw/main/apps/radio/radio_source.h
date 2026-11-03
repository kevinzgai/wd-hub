#pragma once
#include "esp_http_client.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include <cstddef>
#include <cstdint>

// All HTTP ownership remains in one session; the reader is joined before close.
class RadioSource {
public:
    using Cancelled = bool (*)(void *);
    static constexpr size_t kHlsWorkspaceBytes = 6000;
    RadioSource(Cancelled cancelled, void *context, void *workspace = nullptr, size_t workspace_bytes = 0)
        : cancelled_(cancelled), context_(context), workspace_(workspace), workspace_bytes_(workspace_bytes) {}
    ~RadioSource();
    bool open(const char *url);
    int read(uint8_t *data, size_t size);
    bool complete() const { return finished_; }
    bool oom() const { return oom_; }
    uint32_t icy_interval() const { return icy_interval_; }
    esp_audio_simple_dec_type_t type() const { return type_; }
private:
    struct Hls;
    Hls *hls_{};
    esp_http_client_handle_t client_{};
    Cancelled cancelled_;
    void *context_;
    void *workspace_{};
    size_t workspace_bytes_{};
    bool finished_{}, oom_{};
    uint32_t icy_interval_{};
    char mime_[80]{};
    uint8_t prefix_[564]{};
    size_t prefix_size_{}, prefix_pos_{};
    esp_audio_simple_dec_type_t type_{ESP_AUDIO_SIMPLE_DEC_TYPE_MP3};
    bool cancelled() const { return cancelled_(context_); }
    static esp_err_t event(esp_http_client_event_t *event);
    bool request(const char *url);
    void close_http();
    bool playlist(bool initial, bool already_open = false);
    bool next_segment();
};
