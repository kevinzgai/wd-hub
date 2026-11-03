#include "registry_platform.h"
#include <cstring>
#include "../main/launcher/app_registry.cc"
static int starts,stops,keys;
static lv_obj_t *screens[APP_ID_COUNT]{};
static void start(){++starts;screens[app_registry_get_active()]=lv_obj_create(nullptr);lv_screen_load(screens[app_registry_get_active()]);}
static void stop(){++stops;auto &screen=screens[app_registry_get_active()];if(screen){lv_obj_delete(screen);screen=nullptr;}}
static bool prepare(){return mock_ready;}
static bool key_opens_launcher=false;
static void key(uint8_t,app_btn_event_t,uint16_t){++keys;if(key_opens_launcher)app_registry_switch_to(APP_ID_LAUNCHER);}
static passport_app_t app(passport_app_id_t id){passport_app_t a{};a.id=id;a.name="test";a.start=start;a.stop=stop;a.on_key=key;a.prepare_stop=prepare;return a;}
const passport_app_t g_radio_app=app(APP_ID_RADIO),g_xiaozhi_app=app(APP_ID_XIAOZHI),
 g_wifi_app=app(APP_ID_WIFI),g_settings_app=app(APP_ID_SETTINGS),g_home_app=app(APP_ID_HOME),
 g_stopwatch_app=app(APP_ID_STOPWATCH),g_muse_app=app(APP_ID_MUSE),g_pomodoro_app=app(APP_ID_POMODORO),g_morse_app=app(APP_ID_MORSE),
 g_vocab_app=app(APP_ID_VOCAB);
void launcher_ui_show(){start();} void launcher_ui_hide(){stop();}
void launcher_ui_on_key(uint8_t,app_btn_event_t){++keys;}
bool wifi_setup_can_leave(){return mock_setup;}
bool boot_chime_busy(){return mock_chime;}
static bool feedback_ready=true;
static int feedback_count=0;
static ui_sfx_kind_t last_feedback=UI_SFX_NAV;
void ui_sfx_play(ui_sfx_kind_t kind){++feedback_count;last_feedback=kind;}
bool ui_sfx_prepare_stop(){return feedback_ready;}
static int alarm_attempts=0;
bool ui_sfx_alarm_tick(){++alarm_attempts;return true;}
static char overlay_text[64];
void system_overlay_show(const char *text,uint32_t){std::snprintf(overlay_text,sizeof(overlay_text),"%s",text);}
void system_overlay_hide(){overlay_text[0]=0;}
int main(){
 app_registry_init();app_registry_switch_to(APP_ID_HOME);assert(starts==1);
 assert(mock_socket_cleanups==2);
 auto *old=mock_active;const int before_stops=stops;
 mock_ready=false;app_registry_switch_to(APP_ID_RADIO);
 for(int i=0;i<50;++i)app_registry_process_pending_switch();
 assert(starts==1&&stops==before_stops&&mock_active==old);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_CLICK,0);assert(keys==0);
 // Latest request wins; unknown IDs cannot consume a valid pending request.
 app_registry_switch_to(APP_ID_XIAOZHI);app_registry_switch_to(APP_ID_COUNT);
 assert(s_pending_target==APP_ID_XIAOZHI);
 mock_ready=true;app_registry_process_pending_switch();
 assert(starts==2&&stops==before_stops+1&&app_registry_get_active()==APP_ID_XIAOZHI);
 assert(mock_screens.size()==1);
 // A busy display must not discard a request after its worker has stopped.
 mock_display_lock=false;app_registry_switch_to(APP_ID_HOME);assert(starts==2);
 mock_display_lock=true;app_registry_process_pending_switch();assert(starts==3);
 mock_chime=true;app_registry_switch_to(APP_ID_MORSE);assert(starts==3);
 mock_chime=false;app_registry_process_pending_switch();assert(starts==4);
 // Sleep/wake preserves PM lock balance and consumes the waking gesture.
 app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();assert(!app_registry_screen_is_on());
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);assert(app_registry_screen_is_on());
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(keys==0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_RELEASE,0);assert(keys==0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(keys==2);
 assert(!overlay_text[0]);
 // Any key wakes after a long dark interval. Its full gesture is consumed,
 // including long OK, but the next ordinary press immediately reaches the app.
 for (uint8_t button : {BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK}) {
  app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();
  assert(!app_registry_screen_is_on()&&*s_display_awake_lock==0);
  mock_now+=11000000;
  const int before_wake=keys;const auto foreground=app_registry_get_active();
  app_registry_dispatch_key(button,BTN_EVT_PRESS,0);
  assert(app_registry_screen_is_on()&&*s_display_awake_lock==1);
  app_registry_dispatch_key(button,BTN_EVT_LONG,0);
  app_registry_dispatch_key(button,BTN_EVT_LONG_UP,0);
  app_registry_dispatch_key(button,BTN_EVT_CLICK,0);
  app_registry_dispatch_key(button,BTN_EVT_RELEASE,0);
  assert(keys==before_wake&&app_registry_get_active()==foreground&&!overlay_text[0]);
  app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
  app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
  assert(keys==before_wake+2&&app_registry_get_active()==foreground);
 }
 // Waking does not shorten the configured idle timeout to the old 10 s lock timer.
 app_registry_set_screen_timeout(300);mock_now+=11000000;app_registry_tick();
 assert(app_registry_screen_is_on()&&*s_display_awake_lock==1);
 mock_now+=290000000;app_registry_tick();
 assert(!app_registry_screen_is_on()&&*s_display_awake_lock==0);
 // A reminder after a long dark interval is immediately answerable as well.
 mock_now+=11000000;assert(app_registry_wake_for_notice());
 const int before_notice_key=keys;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
 assert(keys==before_notice_key+2&&!overlay_text[0]);

 // Returning to the still-visible app during cancellation must supersede the
 // pending destination and restart the stopped worker, not drop the request.
 mock_ready=false;app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_MORSE);
 assert(s_pending_target==APP_ID_MORSE);mock_ready=true;const int before_restart=starts;
 app_registry_process_pending_switch();assert(starts==before_restart+1&&app_registry_get_active()==APP_ID_MORSE);
 // A reminder wakes the display without changing foreground or its worker.
 app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();assert(!app_registry_screen_is_on());
 const int notice_starts=starts,notice_stops=stops;
 assert(app_registry_wake_for_notice());assert(app_registry_screen_is_on());
 assert(starts==notice_starts&&stops==notice_stops&&app_registry_get_active()==APP_ID_MORSE);
 assert(*s_display_awake_lock==1);assert(app_registry_wake_for_notice());assert(*s_display_awake_lock==1);
 app_registry_switch_to(APP_ID_HOME);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_CLICK,0);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_RELEASE,0);
 assert(feedback_count==1);
 // Clean-launch policy is covered at the end; these flows start from a fresh heap.
 
 feedback_ready=false;app_registry_switch_to(APP_ID_RADIO);
 assert(app_registry_get_active()==APP_ID_HOME&&s_pending_target==APP_ID_RADIO);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(feedback_count==1);
 feedback_ready=true;app_registry_process_pending_switch();assert(app_registry_get_active()==APP_ID_RADIO);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_CLICK,0);assert(feedback_count==1);
 app_registry_switch_to(APP_ID_HOME);
 app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(feedback_count==1);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(feedback_count==2);
 assert(last_feedback==UI_SFX_CONFIRM);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_RELEASE,0);assert(feedback_count==2);
 app_registry_switch_to(APP_ID_LAUNCHER);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG,0);
 assert(app_registry_get_active()==APP_ID_HOME&&feedback_count==3&&last_feedback==UI_SFX_BACK);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG_UP,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_RELEASE,0);assert(feedback_count==3);
 mock_ready=false;key_opens_launcher=true;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
 assert(feedback_count==4&&s_pending_target==APP_ID_LAUNCHER);
 mock_ready=true;key_opens_launcher=false;app_registry_process_pending_switch();
 assert(app_registry_get_active()==APP_ID_LAUNCHER&&feedback_count==4);
 s_building_target=true;assert(!app_registry_wake_for_notice());assert(!app_registry_sound_notice());s_building_target=false;
 assert(app_registry_sound_notice()&&alarm_attempts==1);
 mock_mutex_available=false;assert(!app_registry_sound_notice());mock_mutex_available=true;
 mock_chime=true;assert(!app_registry_sound_notice());mock_chime=false;
 app_registry_switch_to(APP_ID_RADIO);assert(!app_registry_sound_notice()&&alarm_attempts==1);
 // Clean launch: heavy apps start in place while the heap fits them, however
 // often they are re-entered; light apps never restart.
 app_registry_switch_to(APP_ID_HOME);assert(mock_restarts==0);
 app_registry_switch_to(APP_ID_MUSE);assert(app_registry_get_active()==APP_ID_MUSE&&mock_restarts==0);
 app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_XIAOZHI);
 app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_RADIO);
 assert(app_registry_get_active()==APP_ID_RADIO&&mock_restarts==0);
 // An allocation failure during a heavy session marks the heap as strained:
 // leaving is fine, the next heavy entry restarts into itself once.
 ++mock_alloc_failures;app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_STOPWATCH);
 assert(mock_restarts==0);
 const int clean_stops=stops;
 try{app_registry_switch_to(APP_ID_XIAOZHI);assert(false);}catch(const MockRestart&){}
 assert(mock_restarts==1&&stops==clean_stops+1);
 // The panel explains the restart instead of freezing on the old app.
 assert(!std::strcmp(mock_notice,"正在整理内存\n马上打开 test")&&!s_building_target&&!mock_timer.paused);
 // Only a software reset with an intact record is honoured, and only once.
 mock_reset_reason=ESP_RST_SW;assert(app_registry_take_clean_launch()==APP_ID_XIAOZHI);
 assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 s_clean_launch[0]=kCleanLaunchMagic;s_clean_launch[1]=APP_ID_RADIO^kCleanLaunchMagic;
 mock_reset_reason=ESP_RST_PANIC;assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 s_clean_launch[0]=kCleanLaunchMagic;s_clean_launch[1]=APP_ID_STOPWATCH^kCleanLaunchMagic;
 mock_reset_reason=ESP_RST_SW;assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 s_clean_launch[0]=0x12345678;s_clean_launch[1]=APP_ID_RADIO;assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 // Real RAM is fresh after the restart; the mock keeps state, so model that.
 s_heap_strained=false;s_fresh_heap=false;
 // Per-app needs: Xiaozhi fits in 20 KiB contiguous, the MP3 decoder does not.
 mock_largest=20*1024;
 app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_XIAOZHI);
 assert(app_registry_get_active()==APP_ID_XIAOZHI&&mock_restarts==1);
 app_registry_switch_to(APP_ID_HOME);
 try{app_registry_switch_to(APP_ID_RADIO);assert(false);}catch(const MockRestart&){}
 assert(mock_restarts==2);
 // Total free is checked as well as the largest block.
 mock_largest=40000;mock_free=40*1024;
 try{app_registry_switch_to(APP_ID_MUSE);assert(false);}catch(const MockRestart&){}
 assert(mock_restarts==3);
 // The boot after a clean launch never restarts again, even if the heap is short.
 s_clean_launch[0]=kCleanLaunchMagic;s_clean_launch[1]=APP_ID_MUSE^kCleanLaunchMagic;
 assert(app_registry_take_clean_launch()==APP_ID_MUSE);
 app_registry_switch_to(APP_ID_MUSE);assert(app_registry_get_active()==APP_ID_MUSE&&mock_restarts==3);
 mock_largest=40000;mock_free=100000;
 mock_active=nullptr;for(auto *p:mock_screens)delete p;mock_screens.clear();
 delete s_switch_mutex;delete s_display_awake_lock;
 puts("Registry: pending worker gate, feedback ownership, single click, wake gesture, latest target and screen lifecycle, clean launch PASS");
}
