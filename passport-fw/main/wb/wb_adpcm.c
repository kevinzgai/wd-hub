#include "wb_adpcm.h"

// 标准 IMA ADPCM 码表，数值必须与主机 wb_app.py 的 _ADPCM_STEP_TABLE /
// _ADPCM_INDEX_TABLE 完全相同。抄错一位，解码出来的声音就全毁。
static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767,
};

static const int8_t k_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

// 编一个样本，返回 4bit 码，并就地推进 predictor/index。
//
// 必须与主机解码器的累加顺序一致 —— 解码器是：
//   diff = step>>3; if(nib&1) diff += step>>2;
//                   if(nib&2) diff += step>>1;
//                   if(nib&4) diff += step;
// 这里用同样的三项拼出 delta，只是顺序反过来（大的先减）。
// 整数加法可交换，结果逐位相同。
static int encode_one(int sample, int *predictor, int *index)
{
    const int step = k_step[*index];

    int diff = sample - *predictor;
    int code = 0;
    if (diff < 0) {
        code = 8;          // bit3 = 符号位
        diff = -diff;
    }

    int delta = 0;
    if (diff >= step) {
        code |= 4;
        delta += step;
        diff -= step;
    }
    if (diff >= (step >> 1)) {
        code |= 2;
        delta += step >> 1;
        diff -= step >> 1;
    }
    if (diff >= (step >> 2)) {
        code |= 1;
        delta += step >> 2;
    }
    // 解码器的基准项 step/8，即使 nibble 全 0 也会走这么多
    delta += step >> 3;

    *predictor += (code & 8) ? -delta : delta;
    if (*predictor > 32767)  *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;

    *index += k_index[code];
    if (*index < 0)  *index = 0;
    if (*index > 88) *index = 88;

    return code;
}

void wb_adpcm_init(wb_adpcm_state_t *st)
{
    if (!st) return;
    st->predictor = 0;
    st->index = 0;
}

void wb_adpcm_encode_frame(wb_adpcm_state_t *st,
                           const int16_t *samples,
                           uint8_t *out)
{
    if (!st || !samples || !out) return;

    const int predictor0 = (int)st->predictor;
    const int index0 = (int)st->index;

    // 帧头先写：这是**编码前**的快照，解码器靠它重建状态。
    // 必须先写再编码，否则写进去的是编码后的值，解码会整体偏移。
    out[0] = (uint8_t)(predictor0 & 0xFF);
    out[1] = (uint8_t)((predictor0 >> 8) & 0xFF);
    out[2] = (uint8_t)(index0 & 0xFF);

    int predictor = predictor0;
    int index = index0;

    uint8_t *p = out + 3;
    for (int i = 0; i < WB_ADPCM_SAMPLES; i += 2) {
        const int lo = encode_one(samples[i], &predictor, &index);
        const int hi = encode_one(samples[i + 1], &predictor, &index);
        // 低半字节在前 —— 与主机解码器 for nib in (byte & 0x0F, byte >> 4) 对应
        *p++ = (uint8_t)((lo & 0x0F) | ((hi & 0x0F) << 4));
    }

    st->predictor = predictor;
    st->index = index;
}
