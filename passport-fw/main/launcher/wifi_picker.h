#pragma once
#include "apps/app_base.h"
bool wifi_picker_open();
bool wifi_picker_active();
bool wifi_picker_busy();
void wifi_picker_close();
void wifi_picker_key(uint8_t btn, app_btn_event_t event);
