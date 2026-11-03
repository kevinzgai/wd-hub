#pragma once
#include <cstdint>

// Public libopus caller-storage API exported by the pinned esp_audio_codec
// 2.5.0 archive. Keep the decoder opaque; get_size is authoritative at runtime.
// init/decode use the caller's aligned state and never require decoder_create.
extern "C" {
struct OpusDecoder;
struct OpusEncoder;
int opus_encoder_get_size(int channels);
int opus_encoder_init(OpusEncoder *state, int32_t sample_rate, int channels, int application);
int opus_decoder_get_size(int channels);
int opus_decoder_init(OpusDecoder *state, int32_t sample_rate, int channels);
int opus_decode(OpusDecoder *state, const unsigned char *data, int32_t len,
                int16_t *pcm, int frame_size, int decode_fec);
}
