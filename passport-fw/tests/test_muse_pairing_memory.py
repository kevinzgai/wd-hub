"""Exercise production pairing storage and RX framing without a heap buffer."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/passport_muse/sdk/ble_server.c').read_text()
rx = source[source.index('static void rx_reset_locked('):
            source.index('// ---- GATT access callbacks')]
with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    (directory / 'mbedtls').mkdir()
    (directory / 'mbedtls/platform_util.h').write_text(
        '#pragma once\n#include <stddef.h>\n'
        'extern "C" void mbedtls_platform_zeroize(void *, size_t);\n')
    (directory / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include <vector>
#include "bsp_audio_workspace.h"
#include "muse_pairing_memory.h"
extern "C" void mbedtls_platform_zeroize(void *p,size_t n) { memset(p,0,n); }
#define ESP_LOGI(...) ((void)0)
#define MAX_RX_TOTAL_BYTES 8192
#define CHUNK_HEADER_BYTES 3
#define CHUNK_MAGIC 0xfe
#define portMAX_DELAY 0
static int s_rx_mutex=1;
static uint8_t *s_rx_buf, *s_rx_storage;
static size_t s_rx_len, s_rx_cap;
static uint8_t s_rx_total, s_rx_count, s_rx_next_idx;
static void xSemaphoreTake(int,int) {}
static void xSemaphoreGive(int) {}
static unsigned dispatched;
static std::vector<uint8_t> expected;
static void dispatch_command(const uint8_t *data,size_t n) {
    assert(n==expected.size() && !memcmp(data,expected.data(),n)); ++dispatched;
}
''' + rx + r'''
static void chunk(unsigned index,unsigned total,size_t n,uint8_t fill) {
    std::vector<uint8_t> packet(n+3,fill);
    packet[0]=0xfe;packet[1]=index;packet[2]=total;
    handle_rx_write(packet.data(),packet.size());
}
int main() {
    using namespace audio_workspace;
    assert(!muse_pairing_acquire_rx(0) && !muse_pairing_acquire_rx(8193));
    assert(!muse_pairing_cipher_buffer(3075));
    assert(!muse_pairing_decode_buffer(1));
    for(auto app:{Owner::Radio,Owner::Xiaozhi,Owner::Muse}) {
        assert(acquire(app)); memset(storage,0x5a,kBytes);
        assert(!muse_pairing_acquire_rx(8192)); muse_pairing_release_rx();
        assert(!muse_pairing_decode_buffer(4097));
        assert(owner==app); for(auto b:storage)assert(b==0x5a);
        assert(release(app));
    }
    for(unsigned attempt=0;attempt<1000;++attempt) {
        s_rx_storage=muse_pairing_acquire_rx(8192); assert(s_rx_storage==storage);
        assert(!muse_pairing_acquire_rx(8192) && !acquire(Owner::Muse));
        auto *cipher=muse_pairing_cipher_buffer(3075);
        assert(cipher==storage+8192);
        assert(!muse_pairing_cipher_buffer(0) && !muse_pairing_cipher_buffer(3076));
        memset(cipher,0xa5,3075);
        auto *decode=muse_pairing_decode_buffer(4097);
        assert(decode==reinterpret_cast<char *>(storage+8192+3075));
        assert(!muse_pairing_decode_buffer(0) && !muse_pairing_decode_buffer(4098));
        memset(decode,0x5a,4097);
        expected.assign(8192,0x61);
        chunk(0,3,3000,0x61);chunk(1,3,3000,0x61);chunk(2,3,2192,0x61);
        assert(dispatched==2*attempt+1 && !s_rx_buf);
        // Out of order, overflow, zero total and interrupted messages reset.
        chunk(1,2,2,0); assert(!s_rx_buf);
        chunk(0,2,8192,0);chunk(1,2,1,0);assert(!s_rx_buf);
        chunk(0,0,1,0);assert(!s_rx_buf);
        chunk(0,3,10,0);rx_reset();assert(!s_rx_buf);
        expected.assign(3105,0x62);
        for(unsigned i=0;i<13;++i)chunk(i,13,i==12?213:241,0x62);
        assert(dispatched==2*attempt+2);
        for(unsigned i=0;i<3075;++i)assert(cipher[i]==0xa5);
        for(unsigned i=0;i<4097;++i)assert(decode[i]==0x5a);
        assert(muse_pairing_stack()==storage+15376);
        assert(!(reinterpret_cast<uintptr_t>(muse_pairing_stack())&15));
        memset(storage+15376,0x3c,MUSE_PAIRING_STACK_BYTES);
        storage[15376+MUSE_PAIRING_STACK_BYTES]=0x7b;
        rx_reset();s_rx_storage=nullptr;muse_pairing_release_rx();
        assert(owner==Owner::None);
        // Worker stack held decrypted credentials: wiped with the buffers.
        for(unsigned i=0;i<15376+MUSE_PAIRING_STACK_BYTES;++i)assert(storage[i]==0);
        assert(storage[15376+MUSE_PAIRING_STACK_BYTES]==0x7b);
        assert(!muse_pairing_stack());
        assert(!muse_pairing_cipher_buffer(1));
        assert(!muse_pairing_decode_buffer(1));
    }
    // The cloud service can borrow the same memory only after BLE teardown.
    assert(acquire(Owner::Muse));assert(release(Owner::Muse));
}
''')
    binary = directory / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g',
                    '-I' + str(directory),
                    '-I' + str(ROOT / 'components/bsp/include'),
                    '-I' + str(ROOT / 'components/passport_muse/sdk'),
                    str(ROOT / 'components/passport_muse/sdk/muse_pairing_memory.cpp'),
                    str(directory / 'test.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Pairing RX: 1000 lifetimes, framing, bounds, worker stack, ownership, erasure: PASS')
