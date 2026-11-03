#define XIAOZHI_REAL_JSON
#include "mock_platform.h"
#define private public
#include "apps/xiaozhi/xiaozhi_protocol.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#undef private
#include "../main/apps/xiaozhi/xiaozhi_protocol.cc"
const char *const kNsXiaozhi="xiaozhi";
std::string store_get_string(const char *,const char *,const char *) {return "client-id";}
bool store_set_string(const char *,const char *,const std::string &) {return true;}
static int accepted_audio=0;
namespace xiaozhi {
AudioService::AudioService() {} AudioService::~AudioService() {}
AudioService &AudioService::GetInstance() {static AudioService a;return a;}
void AudioService::PushIncomingAudio(const uint8_t *,size_t) {++accepted_audio;}
}
int main() {
    using namespace xiaozhi;Protocol p;auto q=xQueueCreate(1,sizeof(Event));p.SetEventQueue(q);
    const auto original_mac=p.mac_address_, original_client=p.client_id_;
    p.Publish(EvType::AiSentence,"fills the queue");assert(q->entries.size()==1);
    p.Publish(EvType::Connected,nullptr);p.Publish(EvType::HelloOk,nullptr);
    p.Publish(EvType::Disconnected,nullptr);p.Publish(EvType::SocketError,nullptr);
    Event event;const EvType order[]={EvType::Disconnected,EvType::SocketError,EvType::Connected,EvType::HelloOk};
    for(auto expected:order) {assert(p.PopCriticalEvent(&event));assert(event.type==expected);}
    assert(!p.PopCriticalEvent(&event));assert(q->entries.size()==1);
    p.SetState(State::Thinking);p.SetState(State::Speaking);assert(p.GetState()==State::Speaking);
    p.client_=reinterpret_cast<void *>(1);p.hello_ok_=false;
    assert(!p.TriggerStartListen());assert(mock_sent_text.empty());
    p.hello_ok_=true;assert(!p.TriggerStartListen()); // missing session id
    p.session_id_="session";assert(p.TriggerStartListen());assert(mock_sent_text.size()==1);
    const char binary[]={0,0,0,1,1};
    esp_websocket_event_data_t data{binary,5,0,5,2,{0}};
    p.SetState(State::Idle);p.HandleData(&data);assert(accepted_audio==0);
    p.SetState(State::Speaking);p.HandleData(&data);assert(accepted_audio==1);
    p.hello_ok_=false;p.HandleData(&data);assert(accepted_audio==1);
    // Real JSON parser + production callback: a goodbye invalidates the
    // session even if the socket remains open and the text queue is full.
    p.hello_ok_=true;
    const char goodbye[]="{\"type\":\"goodbye\"}";
    esp_websocket_event_data_t bye{goodbye,sizeof(goodbye)-1,0,sizeof(goodbye)-1,1,{0}};
    const unsigned stops=mock_ws_stops, destroys=mock_ws_destroys;
    p.HandleData(&bye);
    assert(p.NeedsRestart() && !p.HandshakeDone() && p.GetState()==State::Disconnected);
    assert(!p.TriggerStartListen());
    assert(p.PopCriticalEvent(&event) && event.type==EvType::Disconnected);
    assert(q->entries.size()==1); // lifecycle event bypasses full display queue
    p.WebsocketEventHandler(&p,nullptr,WEBSOCKET_EVENT_CLOSED,nullptr);
    p.WebsocketEventHandler(&p,nullptr,WEBSOCKET_EVENT_FINISH,nullptr);
    assert(!p.PopCriticalEvent(&event)); // one terminal notification
    assert(mock_ws_stops==stops && mock_ws_destroys==destroys); // never stop in callback
    const char late[]="{\"type\":\"hello\",\"session_id\":\"late\"}";
    esp_websocket_event_data_t late_hello{late,sizeof(late)-1,0,sizeof(late)-1,1,{0}};
    p.HandleData(&late_hello);p.HandleData(&data);
    assert(!p.HandshakeDone() && accepted_audio==1);
    // Clean CLOSE without goodbye and unexpected task FINISH both retire the
    // handle. A transient disconnect still permits SDK auto-reconnection.
    for (int terminal : {WEBSOCKET_EVENT_CLOSED,WEBSOCKET_EVENT_FINISH}) {
        assert(p.Start("wss://example.invalid/","",3));p.hello_ok_=true;
        p.WebsocketEventHandler(&p,nullptr,terminal,nullptr);
        assert(p.NeedsRestart() && !p.HandshakeDone());
        assert(p.PopCriticalEvent(&event) && event.type==EvType::Disconnected);
    }
    assert(p.Start("wss://example.invalid/","",3));
    p.WebsocketEventHandler(&p,nullptr,WEBSOCKET_EVENT_DISCONNECTED,nullptr);
    assert(!p.NeedsRestart());
    p.Stop();assert(!p.PopCriticalEvent(&event));
    assert(p.Start("wss://example.invalid/","",3));
    p.HandleData(&late_hello);
    assert(p.HandshakeDone() && p.session_id()=="late");
    delete q;p.SetEventQueue(nullptr);
    p.Stop();assert(p.mac_address_.empty()&&p.client_id_.empty());
    assert(p.Start("wss://example.invalid/", "", 3));
    assert(p.mac_address_==original_mac&&p.client_id_==original_client);
    p.Stop();assert(p.mac_address_.empty()&&p.client_id_.empty());
    std::puts("Xiaozhi protocol: goodbye/clean close/finish recovery, critical mailbox, stale-audio guards PASS");
}
