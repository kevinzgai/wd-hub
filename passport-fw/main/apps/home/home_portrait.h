#pragma once

#include <stddef.h>
#include <stdint.h>

#define HOME_PORTRAIT_W 96
#define HOME_PORTRAIT_H 138
#define HOME_PORTRAIT_PIXELS (HOME_PORTRAIT_W * HOME_PORTRAIT_H)
#define HOME_AVATAR_V2_BYTES (HOME_PORTRAIT_PIXELS / 4)
#define HOME_PORTRAIT_MASK_BYTES (HOME_PORTRAIT_PIXELS / 8)
#define HOME_PORTRAIT_HEADER_BYTES 8
#define HOME_AVATAR_BYTES (HOME_PORTRAIT_HEADER_BYTES + HOME_AVATAR_V2_BYTES + HOME_PORTRAIT_MASK_BYTES)
#define HOME_AVATAR_LEGACY_BYTES (96 * 150 / 8)
#define HOME_PORTRAIT_I4_BYTES (64 + HOME_PORTRAIT_PIXELS / 2)

/* PT35, preset (0=legacy, 1=dots, 2=flat, 3=hybrid), three reserved bytes.
 * Then MSB-first 2bpp tones and an independent MSB-first visibility mask. */
static inline int home_portrait_valid_header(const uint8_t *data) {
    return data[0] == 'P' && data[1] == 'T' && data[2] == '3' && data[3] == '5' &&
        data[4] <= 3 && !data[5] && !data[6] && !data[7];
}
static inline void home_portrait_init_header(uint8_t *data, uint8_t preset) {
    data[0] = 'P'; data[1] = 'T'; data[2] = '3'; data[3] = '5';
    data[4] = preset; data[5] = data[6] = data[7] = 0;
}
static inline int home_portrait_upload_size(size_t bytes) {
    return bytes == HOME_AVATAR_BYTES || bytes == HOME_AVATAR_V2_BYTES || bytes == HOME_AVATAR_LEGACY_BYTES;
}
static inline uint8_t home_portrait_level(const uint8_t *data, size_t pixel) {
    return (data[HOME_PORTRAIT_HEADER_BYTES + pixel / 4] >> (6 - 2 * (pixel % 4))) & 3;
}
static inline uint8_t home_portrait_index(const uint8_t *data, size_t pixel) {
    if (!(data[HOME_PORTRAIT_HEADER_BYTES + HOME_AVATAR_V2_BYTES + pixel / 8] & (128 >> (pixel % 8)))) return 0;
    const uint8_t level = home_portrait_level(data, pixel);
    if (!data[4]) return level;
    if (data[4] == 1) return level ? 4 : 0;
    return level + 1;
}
