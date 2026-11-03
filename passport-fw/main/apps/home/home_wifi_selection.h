#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include "ssid_manager.h"

// Keep one candidate per saved SSID, ordered by its strongest visible AP.
template <typename AccessPoints>
std::vector<SsidItem> home_wifi_candidates(const std::vector<SsidItem>& saved,
                                         const AccessPoints& aps,
                                         const std::string& selected = {}) {
    struct Candidate { SsidItem network; int rssi; };
    std::vector<Candidate> ranked;
    for (const auto& item : saved) {
        if (!selected.empty()) {
            if (item.ssid == selected) return {item};
            continue;
        }
        int rssi = -128;
        for (const auto& ap : aps) {
            const char* name = reinterpret_cast<const char*>(ap.ssid);
            size_t size = 0;
            while (size < 32 && name[size]) ++size;
            if (item.ssid == std::string(name, size)) rssi = std::max(rssi, int(ap.rssi));
        }
        if (rssi > -128) ranked.push_back({item, rssi});
    }
    if (!selected.empty()) return {};
    // Early scans can contain other APs before the saved network is found;
    // hidden SSIDs also never appear. Let the driver try stored credentials.
    if (ranked.empty()) return saved;
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.rssi > b.rssi;
    });
    std::vector<SsidItem> result;
    for (const auto& item : ranked) result.push_back(item.network);
    return result;
}
