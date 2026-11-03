"""Run the production internet worker against scripted HTTP/network events."""
from pathlib import Path
import subprocess
import tempfile
source=Path('components/esp-wifi-connect/wifi_internet.cc').read_text()
source=source[source.index('uint32_t WifiManager::NetworkHash'):source.index('void WifiManager::SetPreferredSsid')]
prelude=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include "internet_status.h"
struct Saved {std::string ssid;};
struct SsidManager {std::vector<Saved> saved;static SsidManager &GetInstance(){static SsidManager s;return s;}const auto &GetSsidList(){return saved;}};
struct Station {bool connected=true;bool IsConnected(){return connected;}};
struct WifiManager {
 std::mutex mutex_;bool station_active_=true;std::unique_ptr<Station> station_=std::make_unique<Station>();std::string ssid="home";
 std::atomic<InternetStatus> internet_status_{InternetStatus::Checking};std::atomic<unsigned> network_generation_{1};
 std::atomic<bool> internet_worker_{false};std::atomic<uint32_t> excluded_[10]{};
 std::string GetSsid(){return ssid;}bool IsConnected(){return station_active_&&station_->IsConnected();}
 InternetStatus GetInternetStatus(){return internet_status_;}
 static uint32_t NetworkHash(const std::string&);void AllowNetwork(const std::string&);void InvalidateInternet();void CheckInternet();static void InternetWorker(void*);
};
constexpr int ESP_OK=0,pdPASS=1;
static bool allocation_ok=true;static int tasks=0,disconnects=0,notices=0;static bool captive=false;
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*){++tasks;return allocation_ok?pdPASS:0;}
void vTaskDelete(void*){}
void passport_internet_notice(bool c){++notices;captive=c;}
int esp_wifi_disconnect(){++disconnects;return ESP_OK;}
struct esp_http_client_config_t {const char *url;int timeout_ms;bool disable_auto_redirect;int buffer_size;};
struct Http{int status;bool content;};
static std::vector<Http> replies;static size_t cursor=0;static int cleaned=0;
static std::function<void()> network_change;
Http* esp_http_client_init(const esp_http_client_config_t *c){assert(c->timeout_ms==5000&&c->disable_auto_redirect&&c->buffer_size==512);assert(cursor<replies.size());return &replies[cursor++];}
int esp_http_client_open(Http*h,int){if(network_change){auto f=network_change;network_change={};f();}return h->status?ESP_OK:-1;}
int esp_http_client_fetch_headers(Http*){return 0;}
int esp_http_client_get_status_code(Http*h){return h->status;}
int esp_http_client_read(Http*h,char*,int n){assert(n==1);return h->content?1:0;}
void esp_http_client_close(Http*){}
void esp_http_client_cleanup(Http*){++cleaned;}
'''
main=r'''
static void run(WifiManager &w,std::vector<Http> r){replies=r;cursor=0;cleaned=0;notices=0;disconnects=0;w.internet_worker_=true;WifiManager::InternetWorker(&w);assert(cleaned==(int)cursor&&!w.internet_worker_);}
int main(){
 assert(wifi_http_status(200,false)==InternetStatus::NoInternet);
 {WifiManager w;run(w,{{204,false}});assert(w.internet_status_==InternetStatus::Online&&cursor==1&&notices==0);}
 {WifiManager w;run(w,{{200,true},{204,false}});assert(w.internet_status_==InternetStatus::Online&&notices==0);}
 {WifiManager w;run(w,{{0,false},{0,false}});assert(w.internet_status_==InternetStatus::NoInternet&&notices==1&&!captive&&disconnects==0);}
 SsidManager::GetInstance().saved={{"home"},{"phone"}};
 {WifiManager w;run(w,{{302,false},{0,false}});assert(w.internet_status_==InternetStatus::CaptivePortal&&captive&&disconnects==1&&w.excluded_[0]==WifiManager::NetworkHash("home"));}
 {WifiManager w;w.excluded_[0]=WifiManager::NetworkHash("phone");run(w,{{0,false},{0,false}});assert(disconnects==0);w.AllowNetwork("phone");assert(w.excluded_[0]==0);}
 {WifiManager w;network_change=[&]{w.ssid="phone";++w.network_generation_;};run(w,{{302,false},{204,false}});assert(w.internet_status_==InternetStatus::Online&&notices==0&&disconnects==0);}
 {WifiManager w;network_change=[&]{w.station_->connected=false;w.InvalidateInternet();};run(w,{{302,false}});assert(w.internet_status_==InternetStatus::Offline&&notices==0);}
 {WifiManager w;allocation_ok=false;w.CheckInternet();assert(!w.internet_worker_&&w.internet_status_==InternetStatus::NoInternet);allocation_ok=true;}
}
'''
with tempfile.TemporaryDirectory() as temp:
    p=Path(temp)/'test.cc';exe=Path(temp)/'test';p.write_text(prelude+source+main)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-g','-Icomponents/esp-wifi-connect/include',str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Internet probe: 204, portal, timeout, fallback, stale results, disconnect, cleanup, OOM PASS')
