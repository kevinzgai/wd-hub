#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using esp_err_t=int;
constexpr int WIFI_AUTH_OPEN=0;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=0x103,ESP_ERR_NVS_NOT_FOUND=0x1102;
inline const char *esp_err_to_name(int){return "fixture";}
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
constexpr int BIT0=1,BIT1=2,BIT2=4,pdFALSE=0,portTICK_PERIOD_MS=1;
using EventGroupHandle_t=unsigned *;
inline auto xEventGroupCreate(){return new unsigned(0);}
inline void vEventGroupDelete(EventGroupHandle_t g){delete g;}
inline unsigned xEventGroupGetBits(EventGroupHandle_t g){return *g;}
inline unsigned xEventGroupClearBits(EventGroupHandle_t g,unsigned b){return *g&=~b;}
inline unsigned xEventGroupSetBits(EventGroupHandle_t g,unsigned b){return *g|=b;}
inline unsigned xEventGroupWaitBits(EventGroupHandle_t g,unsigned,int,int,int){return *g;}
using esp_event_base_t=const char *;
using esp_event_handler_instance_t=void *;
using Handler=void (*)(void *,esp_event_base_t,int32_t,void *);
inline const char *WIFI_EVENT="wifi",*IP_EVENT="ip";
constexpr int ESP_EVENT_ANY_ID=-1,WIFI_EVENT_STA_START=1,WIFI_EVENT_SCAN_DONE=2,
 WIFI_EVENT_STA_DISCONNECTED=3,WIFI_EVENT_STA_CONNECTED=4,IP_EVENT_STA_GOT_IP=5;
inline Handler mock_handler=nullptr;
inline void *mock_handler_arg=nullptr;
inline int esp_event_handler_instance_register(esp_event_base_t base,int,Handler fn,void *arg,void **out){
 if(base==WIFI_EVENT){mock_handler=fn;mock_handler_arg=arg;}*out=reinterpret_cast<void *>(1);return ESP_OK;
}
inline int esp_event_handler_instance_unregister(esp_event_base_t base,int,void *){
 if(base==WIFI_EVENT)mock_handler=nullptr;return ESP_OK;
}
struct MockTimer{void (*callback)(void *);void *arg;bool active=false;int64_t delay=0;};
using esp_timer_handle_t=MockTimer *;
constexpr int ESP_TIMER_TASK=0;
struct esp_timer_create_args_t{void (*callback)(void *);void *arg;int dispatch_method;const char *name;bool skip_unhandled_events;};
inline int mock_timers=0;
inline int esp_timer_create(const esp_timer_create_args_t *a,MockTimer **out){*out=new MockTimer{a->callback,a->arg};++mock_timers;return ESP_OK;}
inline int esp_timer_stop(MockTimer *t){t->active=false;return ESP_OK;}
inline int esp_timer_delete(MockTimer *t){delete t;--mock_timers;return ESP_OK;}
inline int esp_timer_start_once(MockTimer *t,int64_t delay){assert(t);t->active=true;t->delay=delay;return ESP_OK;}
inline bool esp_timer_is_active(MockTimer *t){return t&&t->active;}
inline int64_t mock_now_us=1;
inline int64_t esp_timer_get_time(){return mock_now_us;}
using wifi_auth_mode_t=int;
struct wifi_ap_record_t{uint8_t ssid[33]={},bssid[6]={},primary=1;int8_t rssi=-80;wifi_auth_mode_t authmode=3;};
struct wifi_config_t { struct {
 uint8_t ssid[33]={},password[65]={},channel=0,bssid[6]={};bool bssid_set=false;
 int scan_method=0,sort_method=0;uint8_t failure_retry_cnt=0,listen_interval=0;
} sta;};
struct wifi_event_sta_disconnected_t{uint8_t reason=1;};
struct ip_event_got_ip_t{struct{uint32_t ip=1;}ip_info;};
constexpr int WIFI_MODE_STA=1,WIFI_IF_STA=0,WIFI_ALL_CHANNEL_SCAN=1,WIFI_CONNECT_AP_BY_SIGNAL=1;
using wifi_ps_type_t=int;
constexpr int WIFI_PS_MAX_MODEM=2,WIFI_PS_MIN_MODEM=1,WIFI_PS_NONE=0;
inline std::vector<wifi_ap_record_t> mock_scan_records;
inline size_t mock_scan_cursor=0;
inline int mock_scan_starts=0,mock_scan_clears=0,mock_connects=0,mock_scan_error=ESP_OK,mock_records_error=ESP_OK;
inline wifi_config_t mock_config;
inline int esp_wifi_scan_start(void *,bool){++mock_scan_starts;return mock_scan_error;}
inline int esp_wifi_scan_stop(){return ESP_OK;}
inline int esp_wifi_scan_get_ap_num(uint16_t *n){*n=mock_scan_records.size();return mock_records_error;}
inline int esp_wifi_scan_get_ap_record(wifi_ap_record_t *r){
 if(mock_scan_cursor>=mock_scan_records.size())return ESP_FAIL;*r=mock_scan_records[mock_scan_cursor++];return ESP_OK;
}
inline int esp_wifi_clear_ap_list(){++mock_scan_clears;mock_scan_records.clear();mock_scan_cursor=0;return ESP_OK;}
inline int esp_wifi_set_mode(int){return ESP_OK;}
inline int esp_wifi_start(){assert(mock_timers>0);if(mock_handler)mock_handler(mock_handler_arg,WIFI_EVENT,WIFI_EVENT_STA_START,nullptr);return ESP_OK;}
inline int esp_wifi_stop(){return ESP_OK;}
inline int esp_wifi_disconnect(){return ESP_OK;}
inline int esp_wifi_set_max_tx_power(int8_t){return ESP_OK;}
inline int esp_wifi_set_ps(int){return ESP_OK;}
inline int esp_wifi_set_config(int,const wifi_config_t *c){mock_config=*c;return ESP_OK;}
inline int esp_wifi_connect(){++mock_connects;return ESP_OK;}
inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t *r){*r={};r->rssi=-35;return ESP_OK;}
using esp_netif_t=int;
inline esp_netif_t mock_netif=1;
constexpr int ESP_IPADDR_TYPE_V4=0;
enum esp_netif_dns_type_t { ESP_NETIF_DNS_MAIN,ESP_NETIF_DNS_BACKUP,ESP_NETIF_DNS_FALLBACK };
struct esp_ip4_addr_t {uint32_t addr=0;};
struct esp_netif_dns_info_t {struct {struct {esp_ip4_addr_t ip4;}u_addr;int type=ESP_IPADDR_TYPE_V4;}ip;};
inline esp_netif_dns_info_t mock_dns[3];
inline int mock_dns_writes=0,mock_dns_error=ESP_OK;
inline int esp_netif_get_dns_info(esp_netif_t *,esp_netif_dns_type_t slot,esp_netif_dns_info_t *out){*out=mock_dns[slot];return ESP_OK;}
inline int esp_netif_set_dns_info(esp_netif_t *,esp_netif_dns_type_t slot,const esp_netif_dns_info_t *info){
 ++mock_dns_writes;if(mock_dns_error!=ESP_OK)return mock_dns_error;mock_dns[slot]=*info;return ESP_OK;
}
inline int esp_netif_str_to_ip4(const char *s,esp_ip4_addr_t *out){
 if(!strcmp(s,"223.5.5.5"))out->addr=0x050505df;
 else if(!strcmp(s,"119.29.29.29"))out->addr=0x1d1d1d77;
 else return ESP_FAIL;return ESP_OK;
}
inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *){return &mock_netif;}
inline esp_netif_t *esp_netif_create_default_wifi_sta(){return &mock_netif;}
inline int esp_netif_set_hostname(esp_netif_t *,const char *){return ESP_OK;}
inline void esp_netif_destroy_default_wifi(esp_netif_t *){}
inline void esp_ip4addr_ntoa(uint32_t *,char *target,size_t bytes){std::strncpy(target,"192.0.2.1",bytes);}
using nvs_handle_t=int;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
inline std::map<std::string,std::string> mock_nvs,mock_pending;
inline int nvs_open(const char *,int mode,int *h){*h=mode;if(mode)mock_pending=mock_nvs;return ESP_OK;}
inline void nvs_close(int){}
inline int nvs_commit(int){mock_nvs=mock_pending;return ESP_OK;}
inline int nvs_get_i8(int,const char *,int8_t *){return ESP_ERR_NVS_NOT_FOUND;}
inline int nvs_get_u8(int,const char *,uint8_t *){return ESP_ERR_NVS_NOT_FOUND;}
inline int nvs_get_str(int,const char *key,char *buf,size_t *size){
 auto it=mock_nvs.find(key);if(it==mock_nvs.end())return ESP_ERR_NVS_NOT_FOUND;
 assert(*size>=it->second.size()+1);std::strcpy(buf,it->second.c_str());*size=it->second.size()+1;return ESP_OK;
}
inline int nvs_set_str(int,const char *key,const char *value){mock_pending[key]=value;return ESP_OK;}
inline int nvs_erase_key(int,const char *key){mock_pending.erase(key);return ESP_OK;}
