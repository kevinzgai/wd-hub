"""Fault-inject the real request producers and drain code, including teardown races."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/passport_muse/sdk/noise_control.cpp').read_text()
queue = source[source.index('enum class req_op_kind'):source.index('static bool req_open(esp_tls_t')]
public = source[source.index('extern "C" const char *noise_ctrl_req_error'):source.index('#endif  // CONFIG_MUSE_ENABLED', source.index('extern "C" const char *noise_ctrl_req_error'))]
prelude = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <deque>
#include <vector>
#include <mutex>
#include <thread>
#include "bsp_audio_workspace.h"
#define SHARED_MUSE_WORKSPACE 1
#define REQ_QUEUE_LEN 8
#define REQ_MAX_STREAMS 4
#define REQ_MAX_HEADERS 16
#define REQ_STREAM_BASE 100
#define REQ_QUEUE_BYTES 2048
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
#define MALLOC_CAP_DMA 1
#define ESP_LOGW(...) ((void)0)
using TickType_t=unsigned;
using StaticSemaphore_t=std::mutex;
using SemaphoreHandle_t=std::mutex*;
static void xSemaphoreTake(std::mutex *m,int){m->lock();}
static void xSemaphoreGive(std::mutex *m){m->unlock();}
using StaticQueue_t=int;
using QueueHandle_t=void*;
using noise_ctrl_req_cb=void(*)(void*,int,const uint8_t*,size_t,bool);
static std::deque<std::vector<uint8_t>> items;
static std::mutex queue_lock;
static size_t item_size;
static std::atomic<bool> s_connected{false};
static std::atomic<unsigned> ticks{0};
static void (*delay_hook)();
static void vTaskDelay(int n){ticks+=n;if(delay_hook){auto h=delay_hook;delay_hook=nullptr;h();}std::this_thread::yield();}
static unsigned xTaskGetTickCount(){return ticks;}
static int64_t esp_timer_get_time(){return (int64_t)ticks*1000;}
static int xQueueSend(void*,const void *p,int){std::lock_guard<std::mutex> lock(queue_lock);if(items.size()==8)return 0;items.emplace_back((uint8_t*)p,(uint8_t*)p+item_size);return 1;}
static int xQueueReceive(void*,void *p,int){std::lock_guard<std::mutex> lock(queue_lock);if(items.empty())return 0;memcpy(p,items.front().data(),item_size);items.pop_front();return 1;}
static unsigned heap_caps_get_free_size(int){return 40000;}
static unsigned heap_caps_get_largest_free_block(int){return 18000;}
static unsigned allocations;
static bool fail_alloc;
static void *test_malloc(size_t n){allocations++;return fail_alloc?nullptr:malloc(n);}
#define malloc test_malloc
'''
checks = r'''
#undef malloc
static unsigned callbacks;
static void callback(void*,int status,const uint8_t*,size_t,bool end){assert(status==-1 && end);callbacks++;}
static void handoff(){
    req_set_ready(false);req_end_all();assert(!s_req_pool_used);
    assert(audio_workspace::release(audio_workspace::Owner::Muse));
    assert(audio_workspace::acquire(audio_workspace::Owner::Radio));
    memset(audio_workspace::storage,0x5a,audio_workspace::kBytes);
}
int main(){
    using namespace audio_workspace;
    s_req_q=&items;s_req_gate=&s_req_gate_storage;item_size=sizeof(req_op);
    assert(acquire(Owner::Muse));req_set_ready(true);
    memset(storage,0xa5,kBytes);uint8_t data[1152];memset(data,0x33,sizeof(data));
    assert(noise_ctrl_req_send(100,data,1024,false,0));
    assert(noise_ctrl_req_send(100,data,1152,true,0));
    assert(!allocations && s_req_queued_bytes==0 && s_req_pool_used==3);
    assert(!noise_ctrl_req_send(100,data,1,false,0));assert(!strcmp(noise_ctrl_req_error(),"QUEUE BUSY"));
    assert(!noise_ctrl_req_send(100,data,1153,false,0));assert(!strcmp(noise_ctrl_req_error(),"PROTOCOL ERROR"));
    for(auto p=storage;p<muse::queue_slot(0);p++)assert(*p==0xa5);
    for(auto p=muse::queue_slot(2);p<storage+kBytes;p++)assert(*p==0xa5);
    req_op op;assert(req_take(&op));assert(!memcmp(op.data,data,op.len));req_free(op.data);
    assert(noise_ctrl_req_send(100,data,1,false,0));
    delay_hook=handoff;assert(!noise_ctrl_req_send(100,data,10,false,200));
    assert(!strcmp(noise_ctrl_req_error(),"NOT READY"));
    for(auto byte:storage)assert(byte==0x5a);
    assert(!noise_ctrl_req_open("POST","/chat",nullptr,false,callback,nullptr));
    assert(release(Owner::Radio));assert(acquire(Owner::Muse));req_set_ready(true);
    fail_alloc=true;assert(!noise_ctrl_req_open("POST","/chat",nullptr,false,callback,nullptr));
    assert(!strcmp(noise_ctrl_req_error(),"OUT OF MEMORY"));fail_alloc=false;
    for(int i=0;i<8;i++)assert(noise_ctrl_req_open("POST","/chat",nullptr,false,callback,nullptr));
    assert(!noise_ctrl_req_send(100,data,100,false,0) && s_req_pool_used==0);
    req_set_ready(false);req_end_all();assert(callbacks==8 && s_req_queued_bytes==0);
    // Race a producer against drain/release. The next owner's bytes must stay intact.
    for(int i=0;i<200;i++){
        req_set_ready(true);
        std::thread producer([&]{noise_ctrl_req_send(100,data,1024,false,0);});
        req_set_ready(false);req_end_all();assert(release(Owner::Muse));
        assert(acquire(Owner::Radio));memset(storage,0x5a,kBytes);
        producer.join();for(auto byte:storage)assert(byte==0x5a);
        assert(!s_req_pool_used && s_req_queued_bytes==0);
        assert(release(Owner::Radio));assert(acquire(Owner::Muse));
    }
    assert(release(Owner::Muse));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'queue.cpp'
    path.write_text(prelude + queue + public + checks)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                    '-Wno-unused-function', '-fsanitize=address,undefined', '-pthread',
                    '-I', str(ROOT / 'components/bsp/include'), str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
print('Muse TX pool: no per-chunk allocations, bounds, backpressure, OOM and 200 teardown races: PASS')
