#include "opus_decoder_storage.h"
#include "audio_workspace.h"
#include "esp_log.h"

// Keep esp_opus_enc's configuration and frame API unchanged. Only its libopus
// factory uses caller storage, through the same pinned public API as decoding.
// AudioService closes the old codec while its worker is idle before reusing it.
extern "C" OpusEncoder *__real_opus_encoder_create(int32_t, int, int, int *);
extern "C" void __real_opus_encoder_destroy(OpusEncoder *);
extern "C" OpusEncoder *__wrap_opus_encoder_create(int32_t rate, int channels,
                                                  int application, int *error) {
    if (audio_workspace::owner.load() != audio_workspace::Owner::Xiaozhi)
        return __real_opus_encoder_create(rate,channels,application,error);
    const int bytes=opus_encoder_get_size(channels);
    if (bytes<=0 || static_cast<size_t>(bytes)>audio_workspace::kAuxBytes) {
        if(error)*error=-7; // OPUS_ALLOC_FAIL; fail before touching the storage.
        return nullptr;
    }
    auto *state=reinterpret_cast<OpusEncoder *>(audio_workspace::aux());
    const int result=opus_encoder_init(state,rate,channels,application);
    if(error)*error=result;
    if(result!=0)return nullptr;
    ESP_LOGI("xiaozhi_audio", "Opus encoder in shared storage: %d bytes",bytes);
    return state;
}
extern "C" void __wrap_opus_encoder_destroy(OpusEncoder *state) {
    if (reinterpret_cast<void *>(state)!=audio_workspace::aux())
        __real_opus_encoder_destroy(state);
}
