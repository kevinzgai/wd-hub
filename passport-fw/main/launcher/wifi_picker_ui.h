#pragma once

#include <cstddef>
#include <cstdint>

struct WifiNetworkView {
    char ssid[33];
    int8_t rssi;
    bool secure;
};

#include "launcher/wifi_keyboard.h"

// Wi-Fi 配网界面：电台应用下架后独立归 launcher 维护。
// 只负责绘制/刷新，网络扫描、连接与按键逻辑都在 wifi_picker.cc。
bool wifi_picker_ui_init();
void wifi_picker_ui_deinit();

void wifi_picker_ui_show_wifi_list(const WifiNetworkView *networks,
                                   std::size_t count, std::size_t selected,
                                   bool scanning, const char *setup_ap_ssid,
                                   bool can_cancel = false);
void wifi_picker_ui_show_wifi_keyboard(const char *ssid,
                                       std::size_t password_length,
                                       WifiKeyboardPage page,
                                       std::size_t selected,
                                       const char *status = nullptr);
void wifi_picker_ui_show_wifi_connecting(const char *ssid, const char *detail);
void wifi_picker_ui_show_wifi_success(const char *ssid);
void wifi_picker_ui_show_saved_wifi(const char *const *ssids,
                                    std::size_t count, std::size_t selected);
void wifi_picker_ui_show_wifi_delete_confirm(const char *ssid,
                                             std::size_t selected);
const char *wifi_picker_ui_wifi_key(WifiKeyboardPage page, std::size_t index);
std::size_t wifi_picker_ui_wifi_key_count(WifiKeyboardPage page);
