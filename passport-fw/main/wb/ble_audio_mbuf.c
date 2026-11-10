#include "ble_audio_mbuf.h"
#include "sdkconfig.h"
#include <stdint.h>

// ATT notification + L2CAP + HCI headers fit in this prepend reserve.
#define AUDIO_HEADROOM 16u

struct os_mbuf *ble_audio_mbuf_from_flat(const void *data, size_t length)
{
    if (!data || !length || length > UINT16_MAX - AUDIO_HEADROOM) return NULL;
    size_t hint = length + AUDIO_HEADROOM;
    // ble_hs_mbuf_from_flat() asks MSYS for a zero-size packet header, which
    // always selects the smallest fitting pool and does not try the next pool
    // when that pool is empty. A raw small-block-size hint exceeds that pool's
    // usable data area and selects the already configured larger pool instead.
    if (hint < CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE) hint = CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE;
    struct os_mbuf *om = os_msys_get_pkthdr((uint16_t)hint, 0);
    if (!om) return NULL;
    const int available = OS_MBUF_TRAILINGSPACE(om);
    if (available < 0 || (size_t)available < length + AUDIO_HEADROOM) {
        os_mbuf_free_chain(om);
        return NULL;
    }
    om->om_data += AUDIO_HEADROOM;
    if (os_mbuf_append(om, data, (uint16_t)length) != 0) {
        os_mbuf_free_chain(om);
        return NULL;
    }
    return om;
}
