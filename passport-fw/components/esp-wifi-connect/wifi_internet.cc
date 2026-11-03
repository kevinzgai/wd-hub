#include "wifi_manager.h"
#include "wifi_configuration_ap.h"
#include "ssid_manager.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "freertos/task.h"

extern "C" void passport_internet_notice(bool captive) __attribute__((weak));

uint32_t WifiManager::NetworkHash(const std::string &ssid) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : ssid) hash = (hash ^ c) * 16777619u;
    return hash ? hash : 1;
}

void WifiManager::InvalidateInternet() {
    ++network_generation_;
    internet_status_ = InternetStatus::Offline;
}

void WifiManager::AllowNetwork(const std::string &ssid) {
    const auto target = NetworkHash(ssid);
    for (auto &hash : excluded_) {
        uint32_t expected = target;
        hash.compare_exchange_strong(expected, 0);
    }
}

void WifiManager::CheckInternet() {
    internet_status_ = InternetStatus::Checking;
    ++network_generation_;
    if (!internet_worker_.exchange(true) &&
        xTaskCreate(InternetWorker, "internet", 4096, this, 2, nullptr) != pdPASS) {
        internet_worker_ = false;
        internet_status_ = InternetStatus::NoInternet;
    }
}

void WifiManager::InternetWorker(void *arg) {
    auto *self = static_cast<WifiManager *>(arg);
    for (;;) {
        const unsigned generation = self->network_generation_.load();
        const auto ssid = self->GetSsid();
        if (!self->IsConnected()) break;
        InternetStatus result = InternetStatus::NoInternet;
        // Two independent endpoints, no redirects or portal body allocation.
        for (const char *url : {"http://connectivitycheck.gstatic.com/generate_204",
                               "http://www.qualcomm.cn/generate_204"}) {
            esp_http_client_config_t config{};
            config.url = url; config.timeout_ms = 5000;
            config.disable_auto_redirect = true; config.buffer_size = 512;
            auto client = esp_http_client_init(&config);
            if (!client) continue;
            if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
                const int status = esp_http_client_get_status_code(client);
                char byte;
                const auto state = wifi_http_status(status, status == 200 && esp_http_client_read(client, &byte, 1) > 0);
                if (state == InternetStatus::Online || state == InternetStatus::CaptivePortal) result = state;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (result == InternetStatus::Online || generation != self->network_generation_) break;
        }
        bool fallback = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (generation != self->network_generation_) continue;
            if (!self->station_active_ || !self->station_->IsConnected()) break;
            self->internet_status_ = result;
            if (result != InternetStatus::Online) {
                for (const auto &saved : SsidManager::GetInstance().GetSsidList()) {
                    if (saved.ssid == ssid) continue;
                    bool allowed = true;
                    for (const auto &hash : self->excluded_) if (hash == NetworkHash(saved.ssid)) allowed = false;
                    if (allowed) { fallback = true; break; }
                }
                if (fallback) {
                    for (auto &hash : self->excluded_) {
                        uint32_t empty = 0;
                        if (hash.compare_exchange_strong(empty, NetworkHash(ssid))) break;
                    }
                }
            }
        }
        if (result != InternetStatus::Online && passport_internet_notice)
            passport_internet_notice(result == InternetStatus::CaptivePortal);
        if (fallback) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (generation == self->network_generation_ && self->station_active_) esp_wifi_disconnect();
        }
        break;
    }
    self->internet_worker_ = false;
    if (self->GetInternetStatus() == InternetStatus::Checking && self->IsConnected()) self->CheckInternet();
    vTaskDelete(nullptr);
}

void WifiManager::SetPreferredSsid(std::function<std::string()> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (station_) station_->SetPreferredSsid(std::move(provider));
}

int WifiManager::LastConfigDisconnectReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_ap_ ? config_ap_->LastDisconnectReason() : 0;
}

