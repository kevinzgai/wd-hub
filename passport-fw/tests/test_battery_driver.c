#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static uint8_t regs[256];
static int64_t now_us=1000000;
static unsigned reads,writes,mode_writes;
static bool fail_read;
int64_t esp_timer_get_time(void){return now_us;}
void vTaskDelay(unsigned ms){now_us+=(int64_t)ms*1000;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
int xSemaphoreTake(SemaphoreHandle_t s,unsigned t){(void)s;(void)t;return 1;}
int xSemaphoreGive(SemaphoreHandle_t s){(void)s;return 1;}
int bsp_i2c_init(void){return ESP_OK;}
void *bsp_i2c_bus(void){return (void*)1;}
int i2c_master_bus_add_device(void *b,const i2c_device_config_t *c,void **d){(void)b;assert(c->device_address==0x63);*d=(void*)1;return ESP_OK;}
int i2c_master_bus_rm_device(void *d){(void)d;return ESP_OK;}
int i2c_master_transmit_receive(void *d,const uint8_t *r,size_t a,uint8_t *out,size_t n,int t){(void)d;(void)a;(void)t;reads++;if(fail_read)return ESP_FAIL;memcpy(out,regs+*r,n);return ESP_OK;}
int i2c_master_transmit(void *d,const uint8_t *b,size_t n,int t){(void)d;(void)t;assert(n==2);writes++;if(b[0]==8){mode_writes++;assert((b[1]&15)==7);}regs[b[0]]=b[1];return ESP_OK;}
static void sample(int soc,int mv){regs[4]=(uint8_t)soc;unsigned raw=(unsigned)mv*10000/3125;regs[2]=raw>>8;regs[3]=raw;}
int main(void){
 regs[0]=15;regs[8]=0xF7;sample(0,4100);
 assert(bsp_battery_init()==ESP_OK); // still attached after SOC readiness timeout
 assert(regs[8]==7 && (regs[11]&128));
 assert(bsp_battery_soc()==-1 && bsp_battery_mv()==4100);
 unsigned modes=mode_writes;
 now_us+=61000000;bsp_battery_poll();assert(mode_writes==modes+2);
 now_us+=120000000;bsp_battery_poll();assert(mode_writes==modes+2); // no reset loop
 sample(42,3800);bsp_battery_poll();assert(bsp_battery_soc()==42);
 unsigned previous_reads=reads;for(int i=0;i<100;i++){assert(bsp_battery_soc()==42);assert(bsp_battery_mv()==3800);}assert(reads==previous_reads);
 sample(0,3300);bsp_battery_poll();assert(bsp_battery_soc()==0); // real low reading is retained
 sample(255,3800);bsp_battery_poll();assert(bsp_battery_soc()==-1);
 sample(42,0);bsp_battery_poll();assert(bsp_battery_soc()==-1);
 sample(42,3800);fail_read=true;bsp_battery_poll();assert(bsp_battery_soc()==-1 && bsp_battery_mv()==-1);
 fail_read=false;regs[8]=0xF7;bsp_battery_poll();assert(regs[8]==7 && bsp_battery_soc()==-1);
 bsp_battery_poll();assert(bsp_battery_soc()==42);
 (void)writes;puts("Battery driver: profile verification, reserved bits, settling, cached reads, zero-SOC contradiction, one recovery, real low SOC and I2C faults PASS");
}
