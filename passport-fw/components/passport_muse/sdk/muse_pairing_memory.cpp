#include "muse_pairing_memory.h"
#include "bsp_audio_workspace.h"
#include "mbedtls/platform_util.h"

namespace {
constexpr size_t kPairingRxBytes = 8192;
constexpr size_t kCipherBytes = 3075; // Maximum decoded 4096-character envelope.
constexpr size_t kDecodeBytes = 4097;
constexpr size_t kPairingBytes = kPairingRxBytes + kCipherBytes + kDecodeBytes;
// Command worker stack: decrypted credentials pass through it, so it is wiped
// together with the buffers on acquire and release.
constexpr size_t kStackOffset = (kPairingBytes + 15) & ~size_t{15};
constexpr size_t kStackBytes = MUSE_PAIRING_STACK_BYTES;
constexpr size_t kWipeBytes = kStackOffset + kStackBytes;
static_assert(kWipeBytes <= audio_workspace::kRealtimeBytes);
}

extern "C" uint8_t *muse_pairing_acquire_rx(size_t bytes) {
    if (!bytes || bytes > kPairingRxBytes ||
        !audio_workspace::acquire(audio_workspace::Owner::MusePairing)) return nullptr;
    mbedtls_platform_zeroize(audio_workspace::storage, kWipeBytes);
    return audio_workspace::storage;
}

extern "C" uint8_t *muse_pairing_cipher_buffer(size_t bytes) {
    if (!bytes || bytes > kCipherBytes ||
        audio_workspace::owner.load() != audio_workspace::Owner::MusePairing) return nullptr;
    return audio_workspace::storage + kPairingRxBytes;
}

extern "C" char *muse_pairing_decode_buffer(size_t bytes) {
    if (!bytes || bytes > kDecodeBytes ||
        audio_workspace::owner.load() != audio_workspace::Owner::MusePairing) return nullptr;
    return reinterpret_cast<char *>(audio_workspace::storage + kPairingRxBytes + kCipherBytes);
}

extern "C" uint8_t *muse_pairing_stack(void) {
    if (audio_workspace::owner.load() != audio_workspace::Owner::MusePairing) return nullptr;
    return audio_workspace::storage + kStackOffset;
}

extern "C" void muse_pairing_release_rx(void) {
    if (audio_workspace::owner.load() != audio_workspace::Owner::MusePairing) return;
    mbedtls_platform_zeroize(audio_workspace::storage, kWipeBytes);
    audio_workspace::release(audio_workspace::Owner::MusePairing);
}
