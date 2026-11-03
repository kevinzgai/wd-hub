#include "ui_sfx.h"
#include <stddef.h>
#include "ui_sfx_pcm.inc"

static const struct {
    const int16_t *pcm;
    unsigned count;
} kTones[] = {
    {kNav, sizeof(kNav) / sizeof(kNav[0])},
    {kConfirm, sizeof(kConfirm) / sizeof(kConfirm[0])},
    {kBack, sizeof(kBack) / sizeof(kBack[0])},
};

unsigned ui_sfx_samples(ui_sfx_kind_t kind) {
    if (kind == UI_SFX_ALARM) return UI_SFX_RATE * 3 / 2;
    if (kind < UI_SFX_NAV || kind > UI_SFX_BACK) return 0;
    return kTones[kind].count;
}

int16_t ui_sfx_sample(ui_sfx_kind_t kind, unsigned index) {
    if (index >= ui_sfx_samples(kind)) return 0;
    if (kind == UI_SFX_ALARM) {
        // Three pairs of envelope-shaped chimes, without a PCM allocation.
        const unsigned in_group = index % (UI_SFX_RATE / 2);
        if (in_group >= 3840) return 0;
        const unsigned in_chime = in_group % 1920;
        return in_chime < kTones[UI_SFX_CONFIRM].count ? kTones[UI_SFX_CONFIRM].pcm[in_chime] : 0;
    }
    return kTones[kind].pcm[index];
}
