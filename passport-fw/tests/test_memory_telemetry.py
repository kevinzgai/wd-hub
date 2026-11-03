"""Execute bounded failure recording and sampled phase minima without an allocator."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = re.sub(r'^#include .*$', '', (ROOT / 'components/bsp/src/bsp_memory.c').read_text(), flags=re.M)
prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
static int locked,logs,walks;
#define portENTER_CRITICAL_SAFE(x) do{(void)(x);assert(!locked);locked++;}while(0)
#define portEXIT_CRITICAL_SAFE(x) do{(void)(x);assert(locked);locked--;}while(0)
#define ESP_LOGI(tag,...) do{(void)(tag);assert(!locked);logs++;snprintf(last,sizeof(last),__VA_ARGS__);}while(0)
#define ESP_LOGW ESP_LOGI
static char last[200];
static size_t total=30000,largest=17000,dma=25000;
static size_t heap_caps_get_free_size(int caps){assert(!locked);walks++;return caps==MALLOC_CAP_DMA?dma:total;}
static size_t heap_caps_get_largest_free_block(int caps){assert(!locked);(void)caps;walks++;return largest;}
static void (*hook)(size_t,uint32_t,const char*);
static int heap_caps_register_failed_alloc_callback(void (*cb)(size_t,uint32_t,const char*)){hook=cb;return 0;}
'''
checks = r'''
int main(void){
 bsp_memory_init();assert(hook);bsp_memory_phase("muse.record");
 int empty_logs=logs,empty_walks=walks;
 bsp_memory_dump_recent_failures();assert(logs==empty_logs&&walks==empty_walks);
 total=21000;largest=12000;dma=19000;bsp_memory_sample();
 total=28000;largest=16000;dma=24000;bsp_memory_sample();
 assert(s_min_free==21000&&s_min_largest==12000&&s_min_dma==19000);
 int before_logs=logs,before_walks=walks;
 for(unsigned i=1;i<=9;i++)hook(i*1024,3,"malloc");
 assert(logs==before_logs&&walks==before_walks&&s_failed==9);
 bsp_memory_dump();assert(logs-before_logs==5 && s_reported==9);
 assert(strstr(last,"seq=9") && strstr(last,"bytes=9216") && strstr(last,"muse.record"));
 before_logs=logs;bsp_memory_dump();assert(logs-before_logs==1);
 before_logs=logs;before_walks=walks;
 bsp_memory_dump_recent_failures();assert(logs-before_logs==4 && walks==before_walks && s_reported==9);
 assert(strstr(last,"recent_alloc_failed seq=9"));
 bsp_memory_phase("radio");assert(s_min_free==28000&&s_min_largest==16000);
 hook(4096,4,"heap_caps_malloc");assert(s_failures[1].phase==s_phase);
 before_logs=logs;before_walks=walks;
 bsp_memory_dump_recent_failures();assert(logs-before_logs==4 && walks==before_walks && s_reported==9);
 assert(strstr(last,"seq=10") && strstr(last,"phase=radio"));
 before_logs=logs;bsp_memory_dump();assert(logs-before_logs==2 && s_reported==10);
 before_logs=logs;bsp_memory_dump_recent_failures();assert(logs-before_logs==4 && s_reported==10);
 puts("Memory telemetry: bounded ring, no hook logging/heap walks, phase minima/reset: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'memory.c'
    path.write_text(prelude + source + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
